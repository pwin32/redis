proc log_file_matches {log pattern} {
    set fp [open $log r]
    set content [read $fp]
    close $fp
    string match $pattern $content
}

if {$::tcl_platform(platform) eq "windows"} {
    start_server {tags {"regression network external:skip tls:skip"} omit {bind}} {
        test {Protected mode accepts IPv6 loopback through IOCP} {
            set replies {}
            for {set i 0} {$i < 2} {incr i} {
                set c [redis ::1 [srv 0 port]]
                set failed [catch {
                    set pong [$c ping]
                    set info [$c client info]
                    if {![regexp {addr=\[::1\]:[0-9]+} $info]} {
                        error "unexpected IPv6 client address: $info"
                    }
                    set pong
                } reply]
                catch {$c close}
                if {$failed} {error $reply}
                lappend replies $reply
            }
            set replies
        } {PONG PONG}
    }

    tags {regression external:skip} {
        test {Windows rejects unsupported server I/O threads} {
            foreach options {{--io-threads 4} {--io-threads-do-reads yes}} {
                set cmd [concat \
                    [list $::redis_server_path --port 0 --save ""] $options]
                set failed [catch {exec {*}$cmd 2>@1} output]
                assert {$failed}
                assert_match \
                    "*server I/O threads are not supported on Windows*" $output
            }
        }
    }

    tags {regression qfork aof network slow external:skip tls:skip} {
        test {Windows QFork mapped heap survives concurrent AOF allocations} {
            for {set attempt 1} {$attempt <= 20} {incr attempt} {
                start_server {overrides {
                    persistence-available yes
                    save ""
                    appendonly yes
                    appendfsync everysec
                    auto-aof-rewrite-percentage 0
                    jemalloc-bg-thread yes
                }} {
                    r config resetstat
                    set cmd [list $::redis_benchmark_path \
                        -h [srv host] -p [srv port] -q \
                        -c 50 -P 16 -n 100000 \
                        -r 100000 -d 512 -t set]
                    if {[catch {exec {*}$cmd 2>@1} output]} {
                        fail "attempt $attempt: redis-benchmark failed: [lindex [split $output \n] 0]"
                    }
                    assert_equal PONG [r ping]
                    assert_match \
                        {calls=100000,*,rejected_calls=0,failed_calls=0} \
                        [cmdrstat set r]
                    assert {![log_file_matches [srv stdout] \
                        "*PhysicalMapMemory:*VirtualFree failed*"]}
                }
            }
        }
    }
}

start_server {tags {"regression"}} {
    set slave [srv 0 client]
    set slave_host [srv 0 host]
    set slave_port [srv 0 port]
    set slave_log [srv 0 stdout]
    start_server {} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        # Set the AUTH password
        $master config set requirepass mypwd
        $slave config set masterauth mypwd

        # Start the replication process...
        $slave slaveof $master_host $master_port

        test {Slave is able to sync with master when AUTH is on} {
            wait_for_condition 50 100 {
                [log_file_matches $slave_log "*Finished with success*"]
            } else {
                fail "Slave is not able to sync with master when AUTH is on"
            }
        }
    }
}

start_server {tags {"regression"}} {
    set A [srv 0 client]
    set A_host [srv 0 host]
    set A_port [srv 0 port]
 
    set max_clients 5
    set arg [format {overrides {maxclients %d requirepass foobar}} $max_clients]
    start_server $arg {
        set B [srv 0 client]
        set B_host [srv 0 host]
        set B_port [srv 0 port]
        
        # Make server A a slave of server B
        $A slaveof $B_host $B_port
        
        test {Master should release the connection after an AUTH failure from a Slave} {
            # Verify A changed role
            wait_for_condition 50 100 {
                [lindex [$A role] 0] eq {slave}
            } else {
                fail {"Can't turn the instance into a slave"}
            }        

            # Wait for multiple connections from A to B
            after 5000
            
            # List all the clients connected to B
            r auth foobar
            set client_count 0
            set client_list [r client list]
            foreach item $client_list {
                if ([string match "id=*" $item]) {
                    incr client_count
                }
            }
            assert {$client_count < $max_clients}
        } 
    }
}
