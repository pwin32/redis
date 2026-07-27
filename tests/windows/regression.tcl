proc log_file_matches {log pattern} {
    set fp [open $log r]
    set content [read $fp]
    close $fp
    string match $pattern $content
}

if {$::tcl_platform(platform) eq "windows"} {
    proc windows_cluster_lock_contender {cluster_config_file} {
        set parent_config [dict get [lindex $::servers end] config]
        # Keep the launcher/config argument relative. The Windows command-line
        # compatibility layer resolves repository-relative config paths; an
        # already-normalized E:/ path would be prefixed a second time.
        set contender_dir [tmpdir cluster-lock-contender]
        set contender_config_file [tmpfile cluster-lock-contender.conf]
        set contender_stdout [file join $contender_dir stdout]
        set contender_stderr [file join $contender_dir stderr]
        set contender_port [find_available_port $::baseport $::portcount]

        set contender_config $parent_config
        dict set contender_config dir $contender_dir
        dict set contender_config port $contender_port
        dict set contender_config cluster-config-file $cluster_config_file
        create_server_config_file $contender_config_file $contender_config {}

        set pid {}
        set contender {}
        set caught [catch {
            # This intentionally failing process is not a normal harness
            # server. Launch it directly through the native no-window helper
            # so its short lifecycle cannot interleave server-tracking packets
            # with the enclosing test result on the harness socket.
            set launch_cmd [list $::redis_test_launcher_path \
                $contender_stdout $contender_stderr -- \
                $::redis_server_path $contender_config_file]
            set pid [string trim [exec {*}$launch_cmd]]
            if {![string is wideinteger -strict $pid] || $pid <= 0} {
                error "hidden Redis launcher returned an invalid PID: $pid"
            }
            set contender [dict create \
                pid $pid \
                host $::host \
                port $contender_port \
                config $contender_config \
                config_file $contender_config_file \
                stdout $contender_stdout \
                stderr $contender_stderr]

            set exited 0
            # Match the normal harness startup allowance.  QFork heap setup
            # and antivirus scanning can delay this deliberately failing
            # contender well beyond two seconds on a loaded Windows host.
            for {set attempt 0} {$attempt < 1200} {incr attempt} {
                if {![process_is_alive $pid]} {
                    set exited 1
                    break
                }
                after 100
            }
            if {!$exited} {
                fail "second Redis process did not reject the locked cluster configuration"
            }

            set output {}
            foreach logfile [list $contender_stdout $contender_stderr] {
                if {[file exists $logfile]} {
                    set fp [open $logfile r]
                    append output [read $fp]
                    close $fp
                }
            }
            assert_match \
                {*already used by a different Redis Cluster node*} $output
            set output
        } result options]

        if {$pid ne {} && [process_is_alive $pid]} {
            if {![windows_process_matches $pid $contender]} {
                error "refusing to terminate unexpected contender PID $pid"
            }
            windows_kill_proc2_checked $pid
            wait_for_condition 100 20 {
                ![process_is_alive $pid]
            } else {
                error "cluster lock contender PID $pid did not exit"
            }
        }
        catch {file delete -force $contender_config_file $contender_dir}

        if {$caught} {
            return -options $options $result
        }
        return $result
    }

    # Cluster mode has only database 0; keep the generic server harness from
    # issuing its usual SELECT 9 while this one cluster-enabled server runs.
    set old_singledb $::singledb
    set ::singledb 1
    set cluster_config_dir [file normalize [tmpdir cluster-config-replacement]]
    set cluster_config_file [file normalize \
        [file join $cluster_config_dir nodes.conf]]
    start_server [list \
        tags {windows cluster regression external:skip tls:skip} \
        overrides [list \
            cluster-enabled yes \
            cluster-node-timeout 1000 \
            cluster-config-file $cluster_config_file]] {
        test {Windows atomically replaces and continuously locks the cluster configuration} {
            set node_id [r cluster myid]
            assert_match {BUMPED *} [r cluster bumpepoch]

            for {set save 0} {$save < 20} {incr save} {
                assert_equal OK [r cluster saveconfig]
            }

            assert_equal 1 [file exists $cluster_config_file]
            assert_equal 1 [file exists ${cluster_config_file}.lock]
            assert_morethan [file size $cluster_config_file] 0

            set fp [open $cluster_config_file r]
            set cluster_config [read $fp]
            close $fp
            assert_match "*$node_id*" $cluster_config
            assert_match {*vars currentEpoch *} $cluster_config
            assert_equal {} [glob -nocomplain ${cluster_config_file}.tmp-*]

            # Start the contender only after repeated atomic replacements.
            # This catches a Windows lock that remained attached to an old,
            # replaced nodes.conf object rather than to the live path.
            windows_cluster_lock_contender $cluster_config_file
            assert_equal PONG [r ping]
            assert_equal OK [r cluster saveconfig]

            # A graceful shutdown must release the stable companion lock.
            # Restarting from the same nodes.conf also validates that the
            # replaced configuration remains parseable and preserves MYID.
            restart_server 0 true false
            assert_equal $node_id [r cluster myid]
            assert_equal OK [r cluster saveconfig]
            assert_equal {} [glob -nocomplain ${cluster_config_file}.tmp-*]
        }
    }
    catch {file delete -force $cluster_config_dir}
    set ::singledb $old_singledb

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

        test {Windows rearms the background I/O completion pipe after FLUSHDB} {
            r mset a 1 b 2 c 3 d 4 e 5 f 6 g 7 h 8 i 9 j 10
            assert_equal OK [r flushdb]
            r set after-flush value
            assert_equal OK [r flushdb]
            assert_equal 0 [r dbsize]
            assert_equal PONG [r ping]
        }
    }

    tags {regression external:skip} {
        test {Windows rejects unsupported server I/O threads} {
            set output [redis_server_startup_error \
                --port 0 --save "" --io-threads 4]
            assert_match \
                "*server I/O threads are not supported on Windows*" $output
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
    start_server {overrides {requirepass mypwd}} {
        set master [srv 0 client]
        set master_host [srv 0 host]
        set master_port [srv 0 port]

        # Keep the password in the harness config as well as Redis runtime
        # state so Windows cleanup can authenticate an exact graceful shutdown.
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
