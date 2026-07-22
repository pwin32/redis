set ::global_overrides {}
set ::tags {}
set ::valgrind_errors {}

# Resolve all test executables and modules through the runner-provided paths.
# The MinGW build keeps its native PE files under build/mingw64, while the
# upstream harness assumes src/*.  Keeping this indirection here means tests
# can retain upstream command content without accidentally launching the
# unrelated installed Redis service or loading source-tree modules.
set ::redis_test_root [file normalize [file join [file dirname [info script]] ../..]]

proc redis_test_binary {envvar relative_path} {
    if {[info exists ::env($envvar)] && [string length $::env($envvar)] > 0} {
        return [file normalize $::env($envvar)]
    }

    set path [file normalize [file join $::redis_test_root {*}$relative_path]]
    if {$::tcl_platform(platform) eq "windows" && [file extension $path] eq {}} {
        append path ".exe"
    }
    return $path
}

proc redis_test_module {name} {
    if {[info exists ::env(REDIS_TEST_MODULE_DIR)] &&
        [string length $::env(REDIS_TEST_MODULE_DIR)] > 0} {
        set module_dir $::env(REDIS_TEST_MODULE_DIR)
    } else {
        set module_dir [file join $::redis_test_root tests modules]
    }
    return [file normalize [file join $module_dir "$name.so"]]
}

set ::redis_server_path [redis_test_binary REDIS_SERVER {src redis-server}]
set ::redis_cli_path [redis_test_binary REDIS_CLI {src redis-cli}]
set ::redis_benchmark_path [redis_test_binary REDIS_BENCHMARK {src redis-benchmark}]
set ::redis_check_aof_path [redis_test_binary REDIS_CHECK_AOF {src redis-check-aof}]
set ::redis_check_rdb_path [redis_test_binary REDIS_CHECK_RDB {src redis-check-rdb}]
set ::redis_test_launcher_path [redis_test_binary REDIS_TEST_LAUNCHER {build mingw64 redis-test-launcher}]

proc start_server_error {config_file error} {
    set err {}
    append err "Can't start the Redis server\n"
    append err "CONFIGURATION:"
    append err [exec cat $config_file]
    append err "\nERROR:"
    append err [string trim $error]
    send_data_packet $::test_server_fd err $err
}

proc check_valgrind_errors stderr {
    set res [find_valgrind_errors $stderr true]
    if {$res != ""} {
        send_data_packet $::test_server_fd err "Valgrind error: $res\n"
    }
}

proc check_sanitizer_errors stderr {
    set res [sanitizer_errors_from_file $stderr]
    if {$res != ""} {
        send_data_packet $::test_server_fd err "Sanitizer error: $res\n"
    }
}

proc clean_persistence config {
    # we may wanna keep the logs for later, but let's clean the persistence
    # files right away, since they can accumulate and take up a lot of space
    set config [dict get $config "config"]
    set dir [dict get $config "dir"]
    set rdb [format "%s/%s" $dir "dump.rdb"]
    if {[dict exists $config "appenddirname"]} {
        set aofdir [dict get $config "appenddirname"]
    } else {
        set aofdir "appendonlydir"
    }
    set aof_dirpath [format "%s/%s" $dir $aofdir]
    clean_aof_persistence $aof_dirpath
    catch {exec rm -rf $rdb}
}

proc kill_server config {
    # nothing to kill when running against external server
    if {$::external} return

    # Close client connection if exists
    if {[dict exists $config "client"]} {
        [dict get $config "client"] close
    }

    # nevermind if its already dead
    if {![is_alive $config]} {
        # Check valgrind errors if needed
        if {$::valgrind} {
            check_valgrind_errors [dict get $config stderr]
        }

        check_sanitizer_errors [dict get $config stderr]
        return
    }
    set pid [dict get $config pid]

    # check for leaks
    if {![dict exists $config "skipleaks"]} {
        catch {
            if {[string match {*Darwin*} [exec uname -a]]} {
                tags {"leaks"} {
                    test "Check for memory leaks (pid $pid)" {
                        set output {0 leaks}
                        catch {exec leaks $pid} output option
                        # In a few tests we kill the server process, so leaks will not find it.
                        # It'll exits with exit code >1 on error, so we ignore these.
                        if {[dict exists $option -errorcode]} {
                            set details [dict get $option -errorcode]
                            if {[lindex $details 0] eq "CHILDSTATUS"} {
                                  set status [lindex $details 2]
                                  if {$status > 1} {
                                      set output "0 leaks"
                                  }
                            }
                        }
                        set output
                    } {*0 leaks*}
                }
            }
        }
    }

    # kill server and wait for the process to be totally exited
    send_data_packet $::test_server_fd server-killing $pid
    kill_proc $config
    if {$::valgrind} {
        set max_wait 120000
    } else {
        set max_wait 10000
    }
    while {[is_alive $config]} {
        incr wait 10

        if {$wait == $max_wait} {
            puts "Forcing process $pid to crash..."
            catch {kill_proc2 $pid}
        } elseif {$wait >= $max_wait * 2} {
            puts "Forcing process $pid to exit..."
            catch {kill_proc2 $pid}
        } elseif {$wait % 1000 == 0} {
            puts "Waiting for process $pid to exit..."
        }
        after 10
    }

    # Check valgrind errors if needed
    if {$::valgrind} {
        check_valgrind_errors [dict get $config stderr]
    }

    check_sanitizer_errors [dict get $config stderr]

    # Remove this pid from the set of active pids in the test server.
    send_data_packet $::test_server_fd server-killed $pid
}

# Return the executable path for one exact Windows PID through the native
# MinGW helper.  The helper deliberately does not invoke PowerShell: these
# queries sit in tight cleanup/polling loops throughout the test harness.
proc windows_process_details {pid} {
    set image [string trim [exec $::redis_test_launcher_path --image $pid]]
    return "$image\n"
}

proc windows_process_matches {pid {config {}}} {
    set expected [file nativename [file normalize $::redis_server_path]]
    # Exact image-path matching is the ownership boundary.  In particular,
    # the installed service under Program Files cannot match the checkout's
    # build/mingw64/redis-server.exe even when a test PID is stale.  Keep the
    # optional config argument for callers and future command-line metadata,
    # but do not reintroduce a PowerShell query into this hot path.
    return [expr {![catch {
        exec $::redis_test_launcher_path --is-owned $pid $expected
    }]}]
}

proc windows_is_alive config {
    return [windows_process_matches [dict get $config pid] $config]
}

proc windows_kill_proc config {
    set pid [dict get $config pid]
    if {![windows_is_alive $config]} { return }

    # Match POSIX SIGTERM by asking Redis to execute its normal shutdown path.
    # If the server is blocked or already stopped, kill_proc2 performs an
    # exact-PID tree termination after the caller's bounded wait.
    set shutdown_client {}
    set shutdown_sent 0
    catch {
        set shutdown_client [redis [dict get $config host] \
                                   [dict get $config port] 1 $::tls]
        set server_config [dict get $config config]
        if {[dict exists $server_config requirepass]} {
            $shutdown_client auth [dict get $server_config requirepass]
        }
        $shutdown_client shutdown
        set shutdown_sent 1
    }
    if {$shutdown_client ne {}} { catch {$shutdown_client close} }

    if {!$shutdown_sent} {
        catch {windows_kill_proc2 $pid}
    }
}

proc windows_process_owned {pid} {
    set allowed {}
    foreach var {redis_server_path redis_cli_path redis_benchmark_path redis_check_aof_path redis_check_rdb_path redis_test_launcher_path} {
        if {[info exists ::$var]} {
            lappend allowed [file nativename [file normalize [set ::$var]]]
        }
    }
    if {[info exists ::tcl_platform(platform)]} {
        lappend allowed [file nativename [file normalize [info nameofexecutable]]]
    }
    return [expr {![catch {
        exec $::redis_test_launcher_path --is-owned $pid {*}$allowed
    }]}]
}

proc windows_kill_proc2 pid {
    if {![windows_process_owned $pid]} { return }
    catch {exec taskkill.exe /F /T /PID $pid}
}

proc windows_kill_proc2_checked pid {
    if {![windows_process_owned $pid]} {
        error "Refusing to terminate unexpected Windows process PID $pid"
    }
    exec taskkill.exe /F /T /PID $pid 2>@1
}

# Run a redis-server invocation that is expected to fail during startup and
# return its combined diagnostics.  On Windows, route even these short-lived
# probes through the native hidden launcher so they cannot flash a console.
# Since the launcher intentionally returns the real Redis PID immediately,
# poll that exact process and retain the same executable-path validation used
# by the normal harness before any forced cleanup.
proc redis_server_startup_error {args} {
    if {$::tcl_platform(platform) ne "windows"} {
        set failed [catch {
            exec $::redis_server_path {*}$args 2>@1
        } output]
        if {!$failed} {
            error "redis-server unexpectedly accepted startup arguments: $args"
        }
        return $output
    }

    set probe_dir [tmpdir redis-server-startup-error]
    set stdout [file join $probe_dir stdout]
    set stderr [file join $probe_dir stderr]
    set pid {}
    set cleanup_error {}

    # The Windows native logger honors --logfile before redis_main() parses
    # the remaining options.  A malformed later directive can therefore put
    # the authoritative fatal-config diagnostic in that file instead of the
    # launcher's stdout/stderr handles.  Remember those paths so the probe
    # returns the same diagnostics that a direct redis-server invocation
    # exposes, without deleting a file that existed before the probe.
    set native_logfiles {}
    set native_logfile_existed {}
    for {set arg_index 0} {$arg_index < [llength $args]} {incr arg_index} {
        set arg [lindex $args $arg_index]
        set logfile_value {}
        if {$arg eq "--logfile"} {
            if {$arg_index + 1 < [llength $args]} {
                incr arg_index
                set logfile_value [lindex $args $arg_index]
            }
        } elseif {[regexp {^--logfile[ \t]+(.+)$} $arg -> logfile_value]} {
            # Redis accepts an option name and value in one argv element.
        }
        if {$logfile_value eq {}} { continue }
        if {[string length $logfile_value] >= 2} {
            set first_char [string index $logfile_value 0]
            set last_char [string index $logfile_value end]
            if {($first_char eq {"} && $last_char eq {"}) ||
                ($first_char eq {'} && $last_char eq {'})} {
                set logfile_value [string range $logfile_value 1 end-1]
            }
        }
        if {$logfile_value eq {} ||
            [string equal -nocase $logfile_value stdout]} {
            continue
        }
        if {[catch {set logfile_path [file normalize $logfile_value]}]} {
            continue
        }
        if {[lsearch -exact $native_logfiles $logfile_path] < 0} {
            lappend native_logfiles $logfile_path
            dict set native_logfile_existed $logfile_path [file exists $logfile_path]
        }
    }

    set caught [catch {
        set launch_cmd [list $::redis_test_launcher_path $stdout $stderr -- \
            $::redis_server_path]
        lappend launch_cmd {*}$args
        set pid [string trim [exec {*}$launch_cmd]]
        if {![string is wideinteger -strict $pid] || $pid <= 0} {
            error "hidden Redis launcher returned an invalid PID: $pid"
        }

        set exited 0
        for {set attempt 0} {$attempt < 300} {incr attempt} {
            if {![process_is_alive $pid]} {
                set exited 1
                break
            }
            after 100
        }
        if {!$exited} {
            error "redis-server unexpectedly remained running for startup arguments: $args"
        }

        set output {}
        foreach logfile [list $stdout $stderr] {
            if {[file exists $logfile]} {
                set fp [open $logfile r]
                append output [read $fp]
                close $fp
            }
        }
        foreach logfile $native_logfiles {
            if {[file exists $logfile] && [file isfile $logfile]} {
                set fp [open $logfile r]
                append output [read $fp]
                close $fp
            }
        }
        set output
    } result options]

    if {$pid ne {} && [process_is_alive $pid]} {
        if {![windows_process_matches $pid]} {
            set cleanup_error \
                "refusing to terminate unexpected startup-probe PID $pid"
        } elseif {[catch {windows_kill_proc2_checked $pid} kill_error]} {
            set cleanup_error $kill_error
        } else {
            set exited 0
            for {set attempt 0} {$attempt < 100} {incr attempt} {
                if {![process_is_alive $pid]} {
                    set exited 1
                    break
                }
                after 20
            }
            if {!$exited} {
                set cleanup_error "startup-probe PID $pid did not exit"
            }
        }
    }
    foreach logfile $native_logfiles {
        if {![dict get $native_logfile_existed $logfile]} {
            catch {file delete -force $logfile}
        }
    }
    catch {file delete -force $probe_dir}

    if {$cleanup_error ne {}} { error $cleanup_error }
    if {$caught} { return -options $options $result }
    return $result
}

if {$::tcl_platform(platform) eq "windows"} {
    proc is_alive config { return [windows_is_alive $config] }
    proc kill_proc config { windows_kill_proc $config }
    proc kill_proc2 pid { windows_kill_proc2 $pid }
    proc kill_proc2_checked pid { windows_kill_proc2_checked $pid }
} else {
    proc is_alive config {
        set pid [dict get $config pid]
        return [expr {![catch {exec kill -0 $pid}]}]
    }
    proc kill_proc config {
        set pid [dict get $config pid]
        catch {exec kill -SIGCONT $pid}
        catch {exec kill $pid}
    }
    proc kill_proc2 pid { catch {exec /bin/kill -9 $pid} }
    proc kill_proc2_checked pid { exec /bin/kill -9 $pid }
}

proc ping_server {host port} {
    set retval 0
    if {[catch {
        if {$::tls} {
            set fd [::tls::socket $host $port] 
        } else {
            set fd [socket $host $port]
        }
        fconfigure $fd -translation binary
        puts $fd "PING\r\n"
        flush $fd
        set reply [gets $fd]
        if {[string range $reply 0 0] eq {+} ||
            [string range $reply 0 0] eq {-}} {
            set retval 1
        }
        close $fd
    } e]} {
        if {$::verbose} {
            puts -nonewline "."
        }
    } else {
        if {$::verbose} {
            puts -nonewline "ok"
        }
    }
    return $retval
}

# Return 1 if the server at the specified addr is reachable by PING, otherwise
# returns 0. Performs a try every 50 milliseconds for the specified number
# of retries.
proc server_is_up {host port retrynum} {
    after 10 ;# Use a small delay to make likely a first-try success.
    set retval 0
    while {[incr retrynum -1]} {
        if {[catch {ping_server $host $port} ping]} {
            set ping 0
        }
        if {$ping} {return 1}
        after 50
    }
    return 0
}

# Check if current ::tags match requested tags. If ::allowtags are used,
# there must be some intersection. If ::denytags are used, no intersection
# is allowed. Returns 1 if tags are acceptable or 0 otherwise, in which
# case err_return names a return variable for the message to be logged.
proc tags_acceptable {tags err_return} {
    upvar $err_return err

    # If tags are whitelisted, make sure there's match
    if {[llength $::allowtags] > 0} {
        set matched 0
        foreach tag $::allowtags {
            if {[lsearch $tags $tag] >= 0} {
                incr matched
            }
        }
        if {$matched < 1} {
            set err "Tag: none of the tags allowed"
            return 0
        }
    }

    foreach tag $::denytags {
        if {[lsearch $tags $tag] >= 0} {
            set err "Tag: $tag denied"
            return 0
        }
    }

    # some units mess with the client output buffer so we can't really use the req-res logging mechanism.
    if {$::log_req_res && [lsearch $tags "logreqres:skip"] >= 0} {
        set err "Not supported when running in log-req-res mode"
        return 0
    }

    if {$::external && [lsearch $tags "external:skip"] >= 0} {
        set err "Not supported on external server"
        return 0
    }

    if {$::singledb && [lsearch $tags "singledb:skip"] >= 0} {
        set err "Not supported on singledb"
        return 0
    }

    if {$::cluster_mode && [lsearch $tags "cluster:skip"] >= 0} {
        set err "Not supported in cluster mode"
        return 0
    }

    if {$::tls && [lsearch $tags "tls:skip"] >= 0} {
        set err "Not supported in tls mode"
        return 0
    }

    if {!$::large_memory && [lsearch $tags "large-memory"] >= 0} {
        set err "large memory flag not provided"
        return 0
    }

    return 1
}

# doesn't really belong here, but highly coupled to code in start_server
proc tags {tags code} {
    # If we 'tags' contain multiple tags, quoted and separated by spaces,
    # we want to get rid of the quotes in order to have a proper list
    set tags [string map { \" "" } $tags]
    set ::tags [concat $::tags $tags]
    if {![tags_acceptable $::tags err]} {
        incr ::num_aborted
        send_data_packet $::test_server_fd ignore $err
        set ::tags [lrange $::tags 0 end-[llength $tags]]
        return
    }
    uplevel 1 $code
    set ::tags [lrange $::tags 0 end-[llength $tags]]
}

# Write the configuration in the dictionary 'config' in the specified
# file name.
proc create_server_config_file {filename config config_lines} {
    set fp [open $filename w+]
    foreach directive [dict keys $config] {
        puts -nonewline $fp "$directive "
        puts $fp [dict get $config $directive]
    }
    foreach {config_line_directive config_line_args} $config_lines {
        puts $fp "$config_line_directive $config_line_args"
    }
    close $fp
}

proc spawn_server {config_file stdout stderr args} {
    set cmd [list $::redis_server_path $config_file]
    set args {*}$args
    if {[llength $args] > 0} {
        lappend cmd {*}$args
    }

    if {$::valgrind} {
        set pid [exec valgrind --track-origins=yes --trace-children=yes --suppressions=[pwd]/src/valgrind.sup --show-reachable=no --show-possibly-lost=no --leak-check=full {*}$cmd >> $stdout 2>> $stderr &]
    } elseif {$::stack_logging && $::tcl_platform(platform) ne "windows"} {
        set pid [exec /usr/bin/env MallocStackLogging=1 MallocLogFile=/tmp/malloc_log.txt {*}$cmd >> $stdout 2>> $stderr &]
    } else {
        # ASAN_OPTIONS environment variable is for address sanitizer. If a test
        # tries to allocate huge memory area and expects allocator to return
        # NULL, address sanitizer throws an error without this setting.
        if {$::tcl_platform(platform) eq "windows"} {
            # Tcl's Windows exec path does not expose CREATE_NO_WINDOW, so a
            # CUI redis-server can allocate a console for every test server.
            # The native launcher creates the actual Redis child with hidden
            # startup flags and assigns these existing log paths directly as
            # inherited standard handles.  It prints the Redis PID (not a
            # wrapper PID), preserving the exact-process cleanup below.
            set launch_cmd [list $::redis_test_launcher_path $stdout $stderr --]
            lappend launch_cmd {*}$cmd
            set pid [string trim [exec {*}$launch_cmd]]
            if {![string is wideinteger -strict $pid] || $pid <= 0} {
                error "hidden Redis launcher returned an invalid PID: $pid"
            }
        } else {
            set pid [exec /usr/bin/env ASAN_OPTIONS=allocator_may_return_null=1 {*}$cmd >> $stdout 2>> $stderr &]
        }
    }

    if {$::wait_server} {
        set msg "server started PID: $pid. press any key to continue..."
        puts $msg
        read stdin 1
    }

    # Tell the test server about this new instance.
    send_data_packet $::test_server_fd server-spawned $pid
    return $pid
}

# Wait for actual startup, return 1 if port is busy, 0 otherwise
proc wait_server_started {config_file stdout pid} {
    set checkperiod 100; # Milliseconds
    set maxiter [expr {120*1000/$checkperiod}] ; # Wait up to 2 minutes.
    set port_busy 0
    while 1 {
        if {[regexp -- " PID: $pid.*Server initialized" [exec cat $stdout]]} {
            break
        }
        after $checkperiod
        incr maxiter -1
        if {$maxiter == 0} {
            start_server_error $config_file "No PID detected in log $stdout"
            puts "--- LOG CONTENT ---"
            puts [exec cat $stdout]
            puts "-------------------"
            break
        }

        # Check if the port is actually busy and the server failed
        # for this reason.
        if {[regexp {Failed listening on port} [exec cat $stdout]]} {
            set port_busy 1
            break
        }
    }
    return $port_busy
}

proc dump_server_log {srv} {
    set pid [dict get $srv "pid"]
    puts "\n===== Start of server log (pid $pid) =====\n"
    puts [exec cat [dict get $srv "stdout"]]
    puts "===== End of server log (pid $pid) =====\n"

    puts "\n===== Start of server stderr log (pid $pid) =====\n"
    puts [exec cat [dict get $srv "stderr"]]
    puts "===== End of server stderr log (pid $pid) =====\n"
}

proc run_external_server_test {code overrides} {
    set srv {}
    dict set srv "host" $::host
    dict set srv "port" $::port
    set client [redis $::host $::port 0 $::tls]
    dict set srv "client" $client
    if {!$::singledb} {
        $client select 9
    }

    set config {}
    dict set config "port" $::port
    dict set srv "config" $config

    # append the server to the stack
    lappend ::servers $srv

    if {[llength $::servers] > 1} {
        if {$::verbose} {
            puts "Notice: nested start_server statements in external server mode, test must be aware of that!"
        }
    }

    r flushall
    r function flush

    # store overrides
    set saved_config {}
    foreach {param val} $overrides {
        dict set saved_config $param [lindex [r config get $param] 1]
        r config set $param $val

        # If we enable appendonly, wait for for rewrite to complete. This is
        # required for tests that begin with a bg* command which will fail if
        # the rewriteaof operation is not completed at this point.
        if {$param == "appendonly" && $val == "yes"} {
            waitForBgrewriteaof r
        }
    }

    if {[catch {set retval [uplevel 2 $code]} error]} {
        if {$::durable} {
            set msg [string range $error 10 end]
            lappend details $msg
            lappend details $::errorInfo
            lappend ::tests_failed $details

            incr ::num_failed
            send_data_packet $::test_server_fd err [join $details "\n"]
        } else {
            # Re-raise, let handler up the stack take care of this.
            error $error $::errorInfo
        }
    }

    # restore overrides
    dict for {param val} $saved_config {
        r config set $param $val
    }

    set srv [lpop ::servers]
    
    if {[dict exists $srv "client"]} {
        [dict get $srv "client"] close
    }
}

proc start_server {options {code undefined}} {
    # setup defaults
    set baseconfig "default.conf"
    set overrides {}
    set omit {}
    set tags {}
    set args {}
    set keep_persistence false
    set config_lines {}

    # parse options
    foreach {option value} $options {
        switch $option {
            "config" {
                set baseconfig $value
            }
            "overrides" {
                set overrides [concat $overrides $value]
            }
            "config_lines" {
                set config_lines $value
            }
            "args" {
                set args $value
            }
            "omit" {
                set omit $value
            }
            "tags" {
                # If we 'tags' contain multiple tags, quoted and separated by spaces,
                # we want to get rid of the quotes in order to have a proper list
                set tags [string map { \" "" } $value]
                set ::tags [concat $::tags $tags]
            }
            "keep_persistence" {
                set keep_persistence $value
            }
            default {
                error "Unknown option $option"
            }
        }
    }

    # We skip unwanted tags
    if {![tags_acceptable $::tags err]} {
        incr ::num_aborted
        send_data_packet $::test_server_fd ignore $err
        set ::tags [lrange $::tags 0 end-[llength $tags]]
        return
    }

    # If we are running against an external server, we just push the
    # host/port pair in the stack the first time
    if {$::external} {
        run_external_server_test $code $overrides

        set ::tags [lrange $::tags 0 end-[llength $tags]]
        return
    }

    set data [split [exec cat "tests/assets/$baseconfig"] "\n"]
    set config {}
    if {$::tls} {
        if {$::tls_module} {
            lappend config_lines [list "loadmodule" [format "%s/src/redis-tls.so" [pwd]]]
        }
        dict set config "tls-cert-file" [format "%s/tests/tls/server.crt" [pwd]]
        dict set config "tls-key-file" [format "%s/tests/tls/server.key" [pwd]]
        dict set config "tls-client-cert-file" [format "%s/tests/tls/client.crt" [pwd]]
        dict set config "tls-client-key-file" [format "%s/tests/tls/client.key" [pwd]]
        dict set config "tls-dh-params-file" [format "%s/tests/tls/redis.dh" [pwd]]
        dict set config "tls-ca-cert-file" [format "%s/tests/tls/ca.crt" [pwd]]
        dict set config "loglevel" "debug"
    }
    foreach line $data {
        if {[string length $line] > 0 && [string index $line 0] ne "#"} {
            set elements [split $line " "]
            set directive [lrange $elements 0 0]
            set arguments [lrange $elements 1 end]
            dict set config $directive $arguments
        }
    }

    # use a different directory every time a server is started
    dict set config dir [tmpdir server]

    # start every server on a different port
    set port [find_available_port $::baseport $::portcount]
    if {$::tls} {
        set pport [find_available_port $::baseport $::portcount]
        dict set config "port" $pport
        dict set config "tls-port" $port
        dict set config "tls-cluster" "yes"
        dict set config "tls-replication" "yes"
    } else {
        dict set config port $port
    }

    set unixsocket {}
    if {$::tcl_platform(platform) ne "windows"} {
        set unixsocket [file normalize [format "%s/%s" [dict get $config "dir"] "socket"]]
        dict set config "unixsocket" $unixsocket
    }

    # apply overrides from global space and arguments
    foreach {directive arguments} [concat $::global_overrides $overrides] {
        dict set config $directive $arguments
    }

    # remove directives that are marked to be omitted
    foreach directive $omit {
        dict unset config $directive
    }

    if {$::log_req_res} {
        dict set config "req-res-logfile" "stdout.reqres"
    }

    if {$::force_resp3} {
        dict set config "client-default-resp" "3"
    }

    # write new configuration to temporary file
    set config_file [tmpfile redis.conf]
    create_server_config_file $config_file $config $config_lines

    set stdout [format "%s/%s" [dict get $config "dir"] "stdout"]
    set stderr [format "%s/%s" [dict get $config "dir"] "stderr"]

    # if we're inside a test, write the test name to the server log file
    if {[info exists ::cur_test]} {
        set fd [open $stdout "a+"]
        puts $fd "### Starting server for test $::cur_test"
        close $fd
        if {$::verbose > 1} {
            puts "### Starting server $stdout for test - $::cur_test"
        }
    }

    # We may have a stdout left over from the previous tests, so we need
    # to get the current count of ready logs
    set previous_ready_count [count_message_lines $stdout "Ready to accept"]

    # We need a loop here to retry with different ports.
    set server_started 0
    while {$server_started == 0} {
        if {$::verbose} {
            puts -nonewline "=== ($tags) Starting server ${::host}:${port} "
        }

        send_data_packet $::test_server_fd "server-spawning" "port $port"

        set pid [spawn_server $config_file $stdout $stderr $args]

        # check that the server actually started
        set port_busy [wait_server_started $config_file $stdout $pid]

        # Sometimes we have to try a different port, even if we checked
        # for availability. Other test clients may grab the port before we
        # are able to do it for example.
        if {$port_busy} {
            puts "Port $port was already busy, trying another port..."
            set port [find_available_port $::baseport $::portcount]
            if {$::tls} {
                set pport [find_available_port $::baseport $::portcount]
                dict set config port $pport
                dict set config "tls-port" $port
            } else {
                dict set config port $port
            }
            create_server_config_file $config_file $config $config_lines

            # Truncate log so wait_server_started will not be looking at
            # output of the failed server.
            close [open $stdout "w"]

            continue; # Try again
        }

        if {$::valgrind} {set retrynum 1000} else {set retrynum 100}
        if {$code ne "undefined"} {
            set serverisup [server_is_up $::host $port $retrynum]
        } else {
            set serverisup 1
        }

        if {$::verbose} {
            puts ""
        }

        if {!$serverisup} {
            set err {}
            append err [exec cat $stdout] "\n" [exec cat $stderr]
            start_server_error $config_file $err
            return
        }
        set server_started 1
    }

    # setup properties to be able to initialize a client object
    set port_param [expr $::tls ? {"tls-port"} : {"port"}]
    set host $::host
    if {[dict exists $config bind]} { set host [dict get $config bind] }
    if {[dict exists $config $port_param]} { set port [dict get $config $port_param] }

    # setup config dict
    dict set srv "config_file" $config_file
    dict set srv "config" $config
    dict set srv "pid" $pid
    dict set srv "host" $host
    dict set srv "port" $port
    dict set srv "stdout" $stdout
    dict set srv "stderr" $stderr
    dict set srv "unixsocket" $unixsocket
    if {$::tls} {
        dict set srv "pport" $pport
    }

    # if a block of code is supplied, we wait for the server to become
    # available, create a client object and kill the server afterwards
    if {$code ne "undefined"} {
        set line [exec head -n1 $stdout]
        if {[string match {*already in use*} $line]} {
            error_and_quit $config_file $line
        }

        while 1 {
            # check that the server actually started and is ready for connections
            if {[count_message_lines $stdout "Ready to accept"] > $previous_ready_count} {
                break
            }
            after 10
        }

        # append the server to the stack
        lappend ::servers $srv

        # connect client (after server dict is put on the stack)
        reconnect

        # remember previous num_failed to catch new errors
        set prev_num_failed $::num_failed

        # execute provided block
        set num_tests $::num_tests
        if {[catch { uplevel 1 $code } error]} {
            set backtrace $::errorInfo
            set assertion [string match "assertion:*" $error]

            # fetch srv back from the server list, in case it was restarted by restart_server (new PID)
            set srv [lindex $::servers end]

            # pop the server object
            set ::servers [lrange $::servers 0 end-1]

            # Kill the server without checking for leaks
            dict set srv "skipleaks" 1
            kill_server $srv

            if {$::dump_logs && $assertion} {
                # if we caught an assertion ($::num_failed isn't incremented yet)
                # this happens when the test spawns a server and not the other way around
                dump_server_log $srv
            } else {
                # Print crash report from log
                set crashlog [crashlog_from_file [dict get $srv "stdout"]]
                if {[string length $crashlog] > 0} {
                    puts [format "\nLogged crash report (pid %d):" [dict get $srv "pid"]]
                    puts "$crashlog"
                    puts ""
                }

                set sanitizerlog [sanitizer_errors_from_file [dict get $srv "stderr"]]
                if {[string length $sanitizerlog] > 0} {
                    puts [format "\nLogged sanitizer errors (pid %d):" [dict get $srv "pid"]]
                    puts "$sanitizerlog"
                    puts ""
                }
            }

            if {!$assertion && $::durable} {
                # durable is meant to prevent the whole tcl test from exiting on
                # an exception. an assertion will be caught by the test proc.
                set msg [string range $error 10 end]
                lappend details $msg
                lappend details $backtrace
                lappend ::tests_failed $details

                incr ::num_failed
                send_data_packet $::test_server_fd err [join $details "\n"]
            } else {
                # Re-raise, let handler up the stack take care of this.
                error $error $backtrace
            }
        } else {
            if {$::dump_logs && $prev_num_failed != $::num_failed} {
                dump_server_log $srv
            }
        }

        # fetch srv back from the server list, in case it was restarted by restart_server (new PID)
        set srv [lindex $::servers end]

        # Don't do the leak check when no tests were run
        if {$num_tests == $::num_tests} {
            dict set srv "skipleaks" 1
        }

        # pop the server object
        set ::servers [lrange $::servers 0 end-1]

        set ::tags [lrange $::tags 0 end-[llength $tags]]
        kill_server $srv
        if {!$keep_persistence} {
            clean_persistence $srv
        }
        set _ ""
    } else {
        set ::tags [lrange $::tags 0 end-[llength $tags]]
        set _ $srv
    }
}

# Start multiple servers with the same options, run code, then stop them.
proc start_multiple_servers {num options code} {
    for {set i 0} {$i < $num} {incr i} {
        set code [list start_server $options $code]
    }
    uplevel 1 $code
}

proc restart_server {level wait_ready rotate_logs {reconnect 1} {shutdown sigterm}} {
    set srv [lindex $::servers end+$level]
    if {$shutdown ne {sigterm}} {
        catch {[dict get $srv "client"] shutdown $shutdown}
    }
    # Kill server doesn't mind if the server is already dead
    kill_server $srv
    # Remove the default client from the server
    dict unset srv "client"

    set pid [dict get $srv "pid"]
    set stdout [dict get $srv "stdout"]
    set stderr [dict get $srv "stderr"]
    if {$rotate_logs} {
        set ts [clock format [clock seconds] -format %y%m%d%H%M%S]
        file rename $stdout $stdout.$ts.$pid
        file rename $stderr $stderr.$ts.$pid
    }
    set prev_ready_count [count_message_lines $stdout "Ready to accept"]

    # if we're inside a test, write the test name to the server log file
    if {[info exists ::cur_test]} {
        set fd [open $stdout "a+"]
        puts $fd "### Restarting server for test $::cur_test"
        close $fd
    }

    set config_file [dict get $srv "config_file"]

    set pid [spawn_server $config_file $stdout $stderr {}]

    # check that the server actually started
    wait_server_started $config_file $stdout $pid

    # update the pid in the servers list
    dict set srv "pid" $pid
    # re-set $srv in the servers list
    lset ::servers end+$level $srv

    if {$wait_ready} {
        while 1 {
            # check that the server actually started and is ready for connections
            if {[count_message_lines $stdout "Ready to accept"] > $prev_ready_count} {
                break
            }
            after 10
        }
    }
    if {$reconnect} {
        reconnect $level
    }
}
