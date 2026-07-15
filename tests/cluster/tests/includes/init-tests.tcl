# Initialization tests -- most units will start including this.

test "(init) Restart killed instances" {
    foreach type {redis} {
        foreach_${type}_id id {
            if {[get_instance_attrib $type $id pid] == -1} {
                puts -nonewline "$type/$id "
                flush stdout
                restart_instance $type $id
            }
        }
    }
}

test "Cluster nodes are reachable" {
    foreach_redis_id id {
        # Every node should be reachable.
        wait_for_condition 1000 50 {
            ([catch {R $id ping} ping_reply] == 0) &&
            ($ping_reply eq {PONG})
        } else {
            catch {R $id ping} err
            fail "Node #$id keeps replying '$err' to PING."
        }
    }
}

test "Cluster replicas are synchronized" {
    # A restarted replica may trigger a QFork-backed full synchronization on
    # its master. Let every sync finish before resetting any cluster node.
    foreach_redis_id id {
        if {[RI $id role] eq {slave}} {
            wait_for_condition 1000 50 {
                [RI $id master_link_status] eq {up}
            } else {
                fail "Replica #$id did not finish synchronizing"
            }
        }
    }
}

test "Cluster nodes hard reset" {
    foreach_redis_id id {
        if {$::valgrind} {
            set node_timeout 10000
        } else {
            set node_timeout 3000
        }
        catch {R $id flushall} ; # May fail for readonly slaves.
        R $id MULTI
        R $id cluster reset hard
        R $id cluster set-config-epoch [expr {$id+1}]
        R $id EXEC
        R $id config set cluster-node-timeout $node_timeout
        R $id config set cluster-slave-validity-factor 10
        R $id config set repl-diskless-load disabled
        R $id config rewrite
    }
}

# Attempt to have each node meet the next one, then wait for auto-discovery.
proc join_nodes_in_cluster {} {
    # Join node 0 with 1, 1 with 2, ... and so forth.
    # If auto-discovery works all nodes will know every other node
    # eventually.
    set ids {}
    foreach_redis_id id {lappend ids $id}

    # Hard resets and replica restarts can invalidate the long-lived harness
    # sockets. Reconnect before each attempt so a stale read cannot block the
    # topology retry itself.
    foreach id $ids {
        set oldlink [Rn $id]
        catch {$oldlink close}
        set host [get_instance_attrib redis $id host]
        set port [get_instance_attrib redis $id port]
        if {[catch {set link [redis $host $port]}]} {
            return 0
        }
        $link reconnect 1
        set_instance_attrib redis $id link $link
    }

    for {set j 0} {$j < [expr [llength $ids]-1]} {incr j} {
        set a [lindex $ids $j]
        set b [lindex $ids [expr $j+1]]
        set b_port [get_instance_attrib redis $b port]
        catch {R $a cluster meet 127.0.0.1 $b_port}
    }

    # A native Windows Tcl connection is relatively expensive. Check the
    # whole topology once per pass instead of opening hundreds of connections
    # to the first node that has not converged yet.
    for {set checks 10} {$checks > 0} {incr checks -1} {
        set joined 1
        foreach id $ids {
            if {[catch {set nodes [get_cluster_nodes_fresh $id connected]}] ||
                [llength $nodes] != [llength $ids]} {
                set joined 0
                break
            }
        }
        if {$joined} {
            return 1
        }
        after 500
    }
    return 0
}

test "Cluster Join and auto-discovery test" {
    # Retry because an individual CLUSTER MEET handshake can time out while
    # nodes are reconnecting after the previous test unit.
    for {set attempts 3} {$attempts > 0} {incr attempts -1} {
        if {[join_nodes_in_cluster]} break
    }
    if {$attempts == 0} {
        fail "Cluster failed to join into a full mesh."
    }
}

test "Before slots allocation, all nodes report cluster failure" {
    assert_cluster_state fail
}
