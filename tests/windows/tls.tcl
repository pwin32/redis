# Native TLS roles use the regular Redis harness and its process cleanup.
if {$::tcl_platform(platform) eq "windows" && $::tls} {
    proc windows_tls_request {args} {
        set request "*[llength $args]\r\n"
        foreach arg $args { append request "\$[string length $arg]\r\n$arg\r\n" }
        return $request
    }

    proc windows_tls_writing {name} {
        set line [lsearch -inline [split [r client list] "\r\n"] "*name=$name*"]
        return [regexp {events=[^ ]*w} $line]
    }

    start_server {tags {windows tls external:skip needs:debug} overrides {save {}}} {
        set tls_args [list --tls --cacert "$::tlsdir/ca.crt" \
            --cert "$::tlsdir/client.crt" --key "$::tlsdir/client.key"]
        set cli [list $::redis_cli_path -h [srv host] -p [srv port] {*}$tls_args]

        test {Windows TLS tools support CLI, rediss URLs, pipes and benchmark pipelines} {
            assert_equal PONG [string trim [exec {*}$cli ping]]
            assert_equal PONG [string trim [exec $::redis_cli_path \
                -u "rediss://[srv host]:[srv port]/0" {*}$tls_args ping]]
            set requests {}
            for {set i 0} {$i < 64} {incr i} {
                append requests [windows_tls_request set windows:tls:pipe:$i $i]
            }
            # Tcl's inline exec redirection translates newlines on Windows.
            set request_file [tmpfile windows-tls-pipe]
            set channel [open $request_file w]
            fconfigure $channel -translation binary
            puts -nonewline $channel $requests
            close $channel
            set output [exec {*}$cli --pipe < $request_file 2>@1]
            file delete $request_file
            assert_match {*errors: 0, replies: 64*} $output
            r config resetstat
            set output [exec $::redis_benchmark_path {*}$tls_args \
                -h [srv host] -p [srv port] -q -n 400 -c 4 -P 8 -t set,get 2>@1]
            assert_match {*SET:*} $output
            assert_match {*GET:*} $output
            assert_match {calls=400,*,rejected_calls=0,failed_calls=0} [cmdrstat set r]
            assert_match {calls=400,*,rejected_calls=0,failed_calls=0} [cmdrstat get r]
        }

        test {Windows TLS benchmark drains multiple records and reconnects} {
            foreach keepalive {1 0} {
                r config resetstat
                exec $::redis_benchmark_path {*}$tls_args -h [srv host] -p [srv port] \
                    -q -n 128 -c 4 -P 8 -d 65536 -k $keepalive -t set,get 2>@1
                assert_match {calls=128,*,rejected_calls=0,failed_calls=0} [cmdrstat set r]
                assert_match {calls=128,*,rejected_calls=0,failed_calls=0} [cmdrstat get r]
            }
        }

        test {Windows TLS preserves binary records, fragments and backpressured replies} {
            set bytes {}
            for {set i 0} {$i < 256} {incr i} {lappend bytes $i}
            set payload [string repeat [binary format c* $bytes] 4096]
            r set windows:tls:binary $payload
            assert_equal $payload [r get windows:tls:binary]
            set client [redis_deferring_client]
            set request [windows_tls_request set windows:tls:fragment [string range $payload 0 1023]]
            for {set offset 0} {$offset < [string length $request]} {incr offset 7} {
                $client write [string range $request $offset [expr {$offset + 6}]]
                $client flush
            }
            assert_equal OK [$client read]
            assert_equal [string range $payload 0 1023] [r get windows:tls:fragment]
            $client client setname windows-tls-slow-reader
            assert_equal OK [$client read]
            for {set i 0} {$i < 16} {incr i} { $client get windows:tls:binary }
            wait_for_condition 100 10 { [windows_tls_writing windows-tls-slow-reader] } else {
                fail "TLS client never reached write backpressure"
            }
            set writes [s total_writes_processed]
            after 300
            assert_lessthan [expr {[s total_writes_processed] - $writes}] 100
            for {set i 0} {$i < 16} {incr i} { assert_equal $payload [$client read] }
            $client close
            r del windows:tls:binary windows:tls:fragment
        }

        test {Windows TLS survives disconnects with unread output and reconnects} {
            r set windows:tls:disconnect [string repeat x 262144]
            for {set i 0} {$i < 24} {incr i} {
                set client [redis_deferring_client]
                $client get windows:tls:disconnect
                $client close
                set client [redis_client]
                assert_equal PONG [$client ping]
                $client close
            }
            r del windows:tls:disconnect
        }

        test {Windows TLS negotiates TLS 1.2 and TLS 1.3 with mutual authentication} {
            foreach version {1.2 1.3} {
                set client [redis [srv host] [srv port] 0 1 \
                    [list -tls1.2 [expr {$version eq "1.2"}] -tls1.3 [expr {$version eq "1.3"}]]]
                assert_equal PONG [$client ping]
                assert_equal "TLSv${version}" [dict get [::tls::status [$client channel]] version]
                $client close
            }
        }

        test {Windows TLS rejects expired and untrusted client certificates} {
            set tlsdir [file normalize $::tlsdir]
            exec openssl req -new -key "$tlsdir/client.key" \
                -subj /CN=expired-client -out "$tlsdir/windows-expired.csr" 2>$::test_null_device
            # Explicit dates work across OpenSSL 3 versions; newer x509
            # commands reject a negative -days interval.
            close [open "$tlsdir/windows-expired-index" w]
            set channel [open "$tlsdir/windows-expired-serial" w]
            puts $channel 9876
            close $channel
            set channel [open "$tlsdir/windows-expired-ca.conf" w]
            puts $channel "\[ca\]\ndefault_ca = test_ca\n\[test_ca\]"
            puts $channel "database = \"$tlsdir/windows-expired-index\""
            puts $channel "serial = \"$tlsdir/windows-expired-serial\""
            puts $channel "new_certs_dir = \"$tlsdir\""
            puts $channel "certificate = \"$tlsdir/ca.crt\""
            puts $channel "private_key = \"$tlsdir/ca.key\""
            puts $channel "default_md = sha256\npolicy = test_policy\n\[test_policy\]\ncommonName = supplied"
            close $channel
            exec openssl ca -batch -config "$tlsdir/windows-expired-ca.conf" \
                -startdate 20200101000000Z -enddate 20200102000000Z \
                -in "$tlsdir/windows-expired.csr" -out "$tlsdir/windows-expired.crt" 2>$::test_null_device
            exec openssl req -new -x509 -key "$tlsdir/client.key" \
                -subj /CN=untrusted-client -days 1 \
                -out "$tlsdir/windows-untrusted.crt" 2>$::test_null_device
            foreach name {expired untrusted} {
                set client [redis [srv host] [srv port] 0 1 \
                    [list -certfile "$tlsdir/windows-$name.crt" -keyfile "$tlsdir/client.key"]]
                assert_equal 1 [catch {$client ping} error]
                $client close
            }
            assert_equal PONG [r ping]
        }

        test {Windows TLS tools reject an untrusted server certificate} {
            foreach tool [list $::redis_cli_path $::redis_benchmark_path] {
                set args [list --tls --cacert "$::tlsdir/windows-untrusted.crt" \
                    --cert "$::tlsdir/client.crt" --key "$::tlsdir/client.key" \
                    -h [srv host] -p [srv port]]
                if {$tool eq $::redis_cli_path} { lappend args ping } else {
                    lappend args -q -n 1 -c 1 -t ping
                }
                assert_equal 1 [catch {exec $tool {*}$args 2>@1} error]
                assert_match {*certificate verify failed*} [string tolower $error]
            }
        }

        test {Windows TLS reloads certificates from paths with spaces and Unicode} {
            set original [r config get tls-cert-file tls-key-file tls-ca-cert-file]
            set directory [file normalize [file join $::tlsdir "renewed certificates \u6d4b\u8bd5"]]
            file mkdir $directory
            foreach name {server.key ca.crt} {
                file copy -force [file join $::tlsdir $name] [file join $directory $name]
            }
            # The OpenSSL CLI's argv encoding depends on the Windows locale.
            # Generate fixtures at ASCII paths, then test Redis's UTF-8 paths.
            set renewed_csr [file join $::tlsdir windows-renewed.csr]
            set renewed_crt [file join $::tlsdir windows-renewed.crt]
            exec openssl req -new -key [file join $::tlsdir server.key] \
                -subj /CN=Windows-renewed -out $renewed_csr 2>$::test_null_device
            exec openssl x509 -req -in $renewed_csr \
                -CA [file join $::tlsdir ca.crt] -CAkey [file join $::tlsdir ca.key] \
                -set_serial 9877 -days 365 -out $renewed_crt 2>$::test_null_device
            file copy -force $renewed_crt [file join $directory server.crt]
            file delete $renewed_csr $renewed_crt
            assert_equal OK [r config set tls-cert-file [encoding convertto utf-8 [file join $directory server.crt]] \
                tls-key-file [encoding convertto utf-8 [file join $directory server.key]] \
                tls-ca-cert-file [encoding convertto utf-8 [file join $directory ca.crt]]]
            set client [redis_client]
            assert_equal PONG [$client ping]
            assert_match {*Windows-renewed*} [dict get [::tls::status [$client channel]] subject]
            $client close
            assert_equal PONG [r ping]
            assert_equal OK [r config set {*}$original]
            file delete -force $directory
        }

        test {Windows QFork persistence keeps established TLS clients alive} {
            r config set appendonly yes
            waitForBgrewriteaof r
            r set windows:tls:persist before
            r bgsave
            waitForBgsave r
            assert_equal PONG [r ping]
            r bgrewriteaof
            waitForBgrewriteaof r
            assert_equal before [r get windows:tls:persist]
            r config set appendonly no
        }

        test {Windows TLS completes graceful shutdown} {
            set shutdown [redis_deferring_client]
            $shutdown shutdown nosave
            wait_for_condition 100 20 { ![windows_is_alive [srv pid]] } else {
                $shutdown close
                fail "TLS shutdown did not stop the server"
            }
            $shutdown close
        }
    }

    # Both TLS and plaintext replicas share the parent's normal connWrite path.
    foreach mode {disk diskless rdb-channel} {
        start_server {tags {windows tls repl external:skip needs:debug} overrides {save {}}} {
            set master [srv client]
            set tls_port [srv port]
            set plain_port [srv pport]
            $master config set repl-diskless-sync [expr {$mode eq "disk" ? "no" : "yes"}]
            $master config set repl-diskless-sync-delay 0
            $master config set repl-rdb-channel [expr {$mode eq "rdb-channel" ? "yes" : "no"}]
            $master set windows:tls:snapshot [string repeat abc\x00 65536]
            start_server {overrides {save {}}} {
                set replica [srv client]
                $replica config set repl-rdb-channel [expr {$mode eq "rdb-channel" ? "yes" : "no"}]
                $replica config set repl-diskless-load [expr {$mode eq "disk" ? "disabled" : "swapdb"}]
                start_server {overrides {save {} tls-replication no}} {
                    set plain [srv client]
                    $plain config set repl-rdb-channel [expr {$mode eq "rdb-channel" ? "yes" : "no"}]
                    test "Windows mixed TLS/TCP full and incremental replication ($mode)" {
                        $replica replicaof 127.0.0.1 $tls_port
                        $plain replicaof 127.0.0.1 $plain_port
                        wait_for_condition 400 50 {
                            [status $replica master_link_status] eq "up" &&
                            [status $plain master_link_status] eq "up"
                        } else { fail "mixed replicas did not synchronize ($mode)" }
                        assert_equal [$master get windows:tls:snapshot] [$replica get windows:tls:snapshot]
                        assert_equal [$master get windows:tls:snapshot] [$plain get windows:tls:snapshot]
                        $master set windows:tls:incremental $mode
                        assert_equal 2 [$master wait 2 10000]
                        assert_equal $mode [$replica get windows:tls:incremental]
                        assert_equal $mode [$plain get windows:tls:incremental]
                    }
                    test "Windows TLS replication recovers an interrupted link ($mode)" {
                        $master client kill type replica
                        $master set windows:tls:after-reconnect $mode
                        wait_for_condition 400 50 {
                            [status $replica master_link_status] eq "up" &&
                            [$replica get windows:tls:after-reconnect] eq $mode
                        } else { fail "TLS replication did not recover ($mode)" }
                        $replica replicaof no one
                        $plain replicaof no one
                    }
                    start_server {overrides {save {}}} {
                        set fresh [srv client]
                        $fresh config set repl-rdb-channel [expr {$mode eq "rdb-channel" ? "yes" : "no"}]
                        test "Windows TLS retries an interrupted full synchronization ($mode)" {
                            for {set i 0} {$i < 256} {incr i} { $master set windows:tls:delay:$i $i }
                            $master config set rdb-key-save-delay 2000
                            $fresh replicaof 127.0.0.1 $tls_port
                            wait_for_condition 200 20 {
                                [status $master rdb_bgsave_in_progress] == 1
                            } else { fail "full synchronization did not start ($mode)" }
                            $master client kill type replica
                            $master config set rdb-key-save-delay 0
                            wait_for_condition 600 50 {
                                [status $fresh master_link_status] eq "up" &&
                                [$fresh get windows:tls:delay:255] eq "255"
                            } else { fail "interrupted full synchronization did not recover ($mode)" }
                            $master set windows:tls:after-full-sync $mode
                            assert_equal 1 [$master wait 1 10000]
                            assert_equal $mode [$fresh get windows:tls:after-full-sync]
                            $fresh replicaof no one
                        }
                    }
                }
            }
        }
    }
}
