if {$::tcl_platform(platform) eq "windows"} {
    tags {windows config external:skip} {
        test {Windows rejects replication compression before starting workers} {
            foreach level {1 22} {
                set output [redis_server_startup_error \
                    --port 0 --save "" --repl-compression $level]
                assert_match {*replication compression is not supported on Windows*} $output
                assert_match {*use repl-compression 0*} $output
                assert_no_match {*Redis is starting*} $output
            }
        }
    }

    start_server {tags {windows config external:skip tls:skip} overrides {
        repl-compression 0
        io-threads 1
        io-threads-do-reads yes
    }} {
        test {Windows accepts safe defaults and ignores the deprecated I/O read directive} {
            assert_equal PONG [r ping]
            assert_equal 1 [lindex [r config get io-threads] 1]
            assert_equal 0 [lindex [r config get repl-compression] 1]
            assert_equal {} [r config get io-threads-do-reads]
        }
    }
}
