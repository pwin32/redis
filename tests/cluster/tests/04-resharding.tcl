# Failover stress test.
# In this test a different node is killed in a loop for N
# iterations. The test checks that certain properties
# are preserved across iterations.

source "../tests/includes/init-tests.tcl"
source "../../../tests/support/cli.tcl"

test "Create a 5 nodes cluster" {
    create_cluster 5 5
}

test "Cluster is up" {
    assert_cluster_state ok
}

test "Enable AOF in all the instances" {
    foreach_redis_id id {
        R $id config set appendonly yes
        # We use "appendfsync no" because it's fast but also guarantees that
        # write(2) is performed before replying to client.
        R $id config set appendfsync no
    }

    foreach_redis_id id {
        wait_for_condition 1000 500 {
            [RI $id aof_rewrite_in_progress] == 0 &&
            [RI $id aof_enabled] == 1
        } else {
            fail "Failed to enable AOF on instance #$id"
        }
    }
}

# Return non-zero if the specified PID is about a process still in execution,
# otherwise 0 is returned.
proc process_is_running {pid} {
    # PS should return with an error if PID is non existing,
    # and catch will return non-zero. We want to return non-zero if
    # the PID exists, so we invert the return value with expr not operator.
    process_is_alive $pid
}

# Our resharding test performs the following actions:
#
# - N commands are sent to the cluster in the course of the test.
# - Every command selects a random key from key:0 to key:MAX-1.
# - The operation RPUSH key <randomvalue> is performed.
# - Tcl remembers into an array all the values pushed to each list.
# - After N/2 commands, the resharding process is started in background.
# - The test continues while the resharding is in progress.
# - At the end of the test, we wait for the resharding process to stop.
# - Finally the keys are checked to see if they contain the value they should.

set numkeys 50000
set numops 200000
set start_node_port [get_instance_attrib redis 0 port]
set cluster [redis_cluster 127.0.0.1:$start_node_port]
if {$::tls} {
    # setup a non-TLS cluster client to the TLS cluster
    set plaintext_port [get_instance_attrib redis 0 plaintext-port]
    set cluster_plaintext [redis_cluster 127.0.0.1:$plaintext_port 0]
    puts "Testing TLS cluster on start node 127.0.0.1:$start_node_port, plaintext port $plaintext_port"
} else {
    set cluster_plaintext $cluster
    puts "Testing using non-TLS cluster"
}
catch {unset content}
array set content {}
set tribpid {}

test "Cluster consistency during live resharding" {
    set ele 0
    for {set j 0} {$j < $numops} {incr j} {
        # Trigger the resharding once we execute half the ops.
        if {$tribpid ne {} &&
            ($j % 10000) == 0 &&
            ![process_is_running $tribpid]} {
            set tribpid {}
        }

        if {$j >= $numops/2 && $tribpid eq {}} {
            puts -nonewline "...Starting resharding..."
            flush stdout
            set target [dict get [get_myself [randomInt 5]] id]
            set tribpid [lindex [exec \
                $::redis_cli_path --cluster reshard \
                127.0.0.1:[get_instance_attrib redis 0 port] \
                --cluster-from all \
                --cluster-to $target \
                --cluster-slots 100 \
                --cluster-yes \
                {*}[rediscli_tls_config "../../../tests"] \
                | [info nameofexecutable] \
                ../tests/helpers/onlydots.tcl \
                &] 0]
        }

        # Write random data to random list.
        set listid [randomInt $numkeys]
        set key "key:$listid"
        incr ele
        # We write both with Lua scripts and with plain commands.
        # This way we are able to stress Lua -> Redis command invocation
        # as well, that has tests to prevent Lua to write into wrong
        # hash slots.
        # We also use both TLS and plaintext connections.
        if {$listid % 3 == 0} {
            $cluster rpush $key $ele
        } elseif {$listid % 3 == 1} {
            $cluster_plaintext rpush $key $ele
        } else {
            $cluster eval {redis.call("rpush",KEYS[1],ARGV[1])} 1 $key $ele
        }
        lappend content($key) $ele

        if {($j % 1000) == 0} {
            puts -nonewline W; flush stdout
        }
    }

    # Wait for the resharding process to end
    wait_for_condition 1000 500 {
        [process_is_running $tribpid] == 0
    } else {
        fail "Resharding is not terminating after some time."
    }

}

test "Verify $numkeys keys for consistency with logical content" {
    # Check that the Redis Cluster content matches our logical content.
    foreach {key value} [array get content] {
        if {[$cluster lrange $key 0 -1] ne $value} {
            fail "Key $key expected to hold '$value' but actual content is [$cluster lrange $key 0 -1]"
        }
    }
}

set ::resharding_original_cluster_node_timeout {}

proc restore_resharding_cluster_node_timeouts {} {
    set first_error {}
    dict for {id timeout} $::resharding_original_cluster_node_timeout {
        if {[catch {R $id config set cluster-node-timeout $timeout} restore_error] &&
            $first_error eq {}} {
            set first_error "Unable to restore cluster-node-timeout on node $id: $restore_error"
        }
        if {[catch {R $id config rewrite} rewrite_error] &&
            $first_error eq {}} {
            set first_error "Unable to persist cluster-node-timeout on node $id: $rewrite_error"
        }
    }
    set ::resharding_original_cluster_node_timeout {}
    return $first_error
}

test "Terminate and restart all the instances" {
    set restart_rc [catch {
        if {$::tcl_platform(platform) eq "windows"} {
            # A rolling restart is materially slower on Windows: each MinGW
            # server must reload its AOF and recreate its IOCP listeners before
            # the next instance is restarted.  Keep the still-starting tail
            # from crossing the normal 3-second failure threshold.  Persist
            # the value because restart_instance reads the on-disk
            # configuration.
            foreach_redis_id id {
                set configured [R $id config get cluster-node-timeout]
                set timeout [lindex $configured 1]
                if {![string is integer -strict $timeout] || $timeout <= 0} {
                    error "Invalid cluster-node-timeout for node $id: $configured"
                }
                dict set ::resharding_original_cluster_node_timeout $id $timeout
                R $id config set cluster-node-timeout 120000
                R $id config rewrite
            }
        }

        foreach_redis_id id {
            # Stop AOF so that an initial AOFRW won't prevent the instance
            # from terminating.
            R $id config set appendonly no
            kill_instance redis $id
            restart_instance redis $id
        }

        if {$::tcl_platform(platform) eq "windows"} {
            # Do not restore the normal timeout until every restarted node has
            # reached the cluster again.
            assert_cluster_state ok
        }
    } restart_error restart_options]

    set restore_error {}
    if {$::tcl_platform(platform) eq "windows" &&
        [dict size $::resharding_original_cluster_node_timeout] > 0} {
        set restore_error [restore_resharding_cluster_node_timeouts]
    }

    if {$restart_rc} {
        if {$restore_error ne {}} {
            append restart_error "\nCleanup error: $restore_error"
        }
        return -options $restart_options $restart_error
    }
    if {$restore_error ne {}} {
        error $restore_error
    }
}

test "Cluster should eventually be up again" {
    if {$::tcl_platform(platform) eq "windows"} {
        # Let one complete normal failure-detection window elapse after the
        # persisted timeout is restored.  A stale-but-present link can look
        # connected until cluster cron has had time to invalidate it.
        after 4000
    }
    assert_cluster_state ok

    if {$::tcl_platform(platform) eq "windows"} {
        # Verify the topology after restoring the normal 3-second timeout.
        # cluster_state alone can remain temporarily optimistic while a dead
        # link is still inside the failure-detection window.
        set expected_nodes 0
        foreach_redis_id id {incr expected_nodes}
        foreach_redis_id id {
            wait_for_condition 600 50 {
                [llength [get_cluster_nodes $id connected]] == $expected_nodes
            } else {
                fail "Cluster node $id does not have a full connected mesh"
            }
        }
    }
}

test "Verify $numkeys keys after the restart" {
    # Check that the Redis Cluster content matches our logical content.
    foreach {key value} [array get content] {
        if {[$cluster lrange $key 0 -1] ne $value} {
            fail "Key $key expected to hold '$value' but actual content is [$cluster lrange $key 0 -1]"
        }
    }
}

test "Disable AOF in all the instances" {
    foreach_redis_id id {
        R $id config set appendonly no
    }
}

test "Verify slaves consistency" {
    set verified_masters 0
    foreach_redis_id id {
        set role [R $id role]
        lassign $role myrole myoffset slaves
        if {$myrole eq {slave}} continue
        set masterport [get_instance_attrib redis $id port]
        set masterdigest [R $id debug digest]
        foreach_redis_id sid {
            set srole [R $sid role]
            if {[lindex $srole 0] eq {master}} continue
            if {[lindex $srole 2] != $masterport} continue
            wait_for_condition 1000 500 {
                [R $sid debug digest] eq $masterdigest
            } else {
                fail "Master and slave data digest are different"
            }
            incr verified_masters
        }
    }
    assert {$verified_masters >= 5}
}

test "Dump sanitization was skipped for migrations" {
    set verified_masters 0
    foreach_redis_id id {
        assert {[RI $id dump_payload_sanitizations] == 0}
    }
}
