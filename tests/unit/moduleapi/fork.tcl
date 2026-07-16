set testmodule [redis_test_module fork]

proc count_fork_log_message {pattern} {
    set result [exec grep -c $pattern < [srv 0 stdout]]
}

start_server {tags {"modules"}} {
    r module load $testmodule

    if {$::tcl_platform(platform) eq "windows"} {
        test {Module fork API is not advertised on Windows} {
            catch {r fork.create 3} err
            set err
        } {*Fork api is not supported*}
    } else {
        test {Module fork} {
            # the argument to fork.create is the exitcode on termination
            r fork.create 3
            wait_for_condition 20 100 {
                [r fork.exitcode] != -1
            } else {
                fail "fork didn't terminate"
            }
            r fork.exitcode
        } {3}

        test {Module fork kill} {
            r fork.create 3
            after 250
            r fork.kill

            assert {[count_fork_log_message "fork child started"] eq "2"}
            assert {[count_fork_log_message "Received SIGUSR1 in child"] eq "1"}
            assert {[count_fork_log_message "fork child exiting"] eq "1"}
        }
    }

}
