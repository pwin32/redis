if {$::tcl_platform(platform) eq "windows"} {
    set ::windows_affinity_helper [file join \
        [file dirname $::redis_test_launcher_path] redis-affinity-test.exe]

    proc windows_affinity_thread_count {pid cpulist} {
        return [string trim [exec $::windows_affinity_helper --count $pid $cpulist]]
    }

    proc windows_assert_qfork_affinity {idx cpulist} {
        set child [get_qfork_child_pid $idx]
        wait_for_condition 300 50 {
            [windows_affinity_thread_count $child $cpulist] >= 1
        } else {
            fail "QFork worker did not apply CPU list $cpulist"
        }
    }

    tags {windows affinity external:skip} {
        test {Windows validates all CPU lists and their aliases before starting workers} {
            foreach setting {
                server-cpulist bio-cpulist bgsave-cpulist aof-rewrite-cpulist
                server_cpulist bio_cpulist bgsave_cpulist aof_rewrite_cpulist
            } {
                set canonical [string map {_ -} $setting]
                foreach invalid {0-1:0 4294967296 0,64} {
                    set output [redis_server_startup_error \
                        --port 0 --save "" --$setting $invalid]
                    assert_match "*Fatal: invalid $canonical*" $output
                    assert_no_match {*Redis is starting*} $output
                }
            }
        }
    }

    set affinity_cpus [string trim [exec $::windows_affinity_helper --cpus]]
    set affinity_main [lindex $affinity_cpus 0]
    set affinity_worker [lindex $affinity_cpus end]
    set affinity_overrides [list \
        server_cpulist $affinity_main \
        bio_cpulist $affinity_worker \
        bgsave_cpulist $affinity_worker \
        aof_rewrite_cpulist $affinity_worker \
        persistence-available yes save "" appendonly yes \
        aof-use-rdb-preamble yes auto-aof-rewrite-percentage 0]

    start_server [list \
        tags {windows affinity qfork external:skip tls:skip needs:debug} \
        overrides $affinity_overrides] {
        waitForBgrewriteaof r

        test {Windows CPU-list aliases remain immutable and apply to main and BIO threads} {
            foreach setting {server-cpulist bio-cpulist bgsave-cpulist aof-rewrite-cpulist} {
                set alias [string map {- _} $setting]
                set expected [dict get $affinity_overrides $alias]
                assert_equal $expected [lindex [r config get $setting] 1]
                assert_equal $expected [lindex [r config get $alias] 1]
                assert_error {*immutable*} [list r config set $setting ""]
            }

            wait_for_condition 100 50 {
                [windows_affinity_thread_count [srv pid] $affinity_main] >= 1 &&
                [windows_affinity_thread_count [srv pid] $affinity_worker] >= 3
            } else {
                fail "main and BIO thread masks do not match the configured CPU lists"
            }
            r debug populate 100 affinity-bio 1024
            r flushdb async
            wait_for_condition 100 50 {
                [s lazyfree_pending_objects] == 0
            } else {
                fail "BIO lazy-free work did not complete with affinity enabled"
            }
            r debug populate 100 affinity-persistence 1024
            assert_equal PONG [r ping]
        }

        # Keep each disposable persistence worker alive long enough to inspect
        # its native thread mask, including on slower Windows CI hosts.
        r config set rdb-key-save-delay 100000

        test {Windows BGSAVE applies its CPU list in the QFork worker} {
            r bgsave
            windows_assert_qfork_affinity 0 $affinity_worker
            waitForBgsave r
            assert_equal ok [s rdb_last_bgsave_status]
            assert {[windows_affinity_thread_count [srv pid] $affinity_main] >= 1}
        }

        test {Windows AOF rewrite applies its CPU list in the QFork worker} {
            r bgrewriteaof
            windows_assert_qfork_affinity 0 $affinity_worker
            waitForBgrewriteaof r
            assert_equal ok [s aof_last_bgrewrite_status]
            set digest [r debug digest]
            r debug loadaof
            assert_equal $digest [r debug digest]
        }

        test {Windows diskless replication applies the BGSAVE CPU list} {
            r config set repl-diskless-sync yes
            r config set repl-diskless-sync-delay 0
            set primary [srv client]
            set primary_host [srv host]
            set primary_port [srv port]
            start_server {overrides {repl-diskless-load on-empty-db}} {
                r replicaof $primary_host $primary_port
                wait_for_condition 300 50 {
                    [s -1 rdb_bgsave_in_progress] == 1
                } else {
                    fail "diskless synchronization did not start"
                }
                windows_assert_qfork_affinity -1 $affinity_worker
                wait_for_condition 600 50 {
                    [s master_link_status] eq "up"
                } else {
                    fail "diskless synchronization did not finish with CPU affinity"
                }
                assert_equal [$primary debug digest] [r debug digest]
            }
            waitForBgsave r
        }

        r config set rdb-key-save-delay 0
        test {Windows persistence leaves the parent's thread affinity intact} {
            assert {[windows_affinity_thread_count [srv pid] $affinity_main] >= 1}
            assert {[windows_affinity_thread_count [srv pid] $affinity_worker] >= 3}
            set log [open [srv stdout] r]
            set output [read $log]
            close $log
            assert_no_match {*Cannot apply CPU affinity*} $output
            assert_equal PONG [r ping]
        }
    }
}
