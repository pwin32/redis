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

        test {Windows CLI and benchmark work with executable ASLR} {
            set cli [list $::redis_cli_path -h [srv host] -p [srv port]]
            assert_equal PONG [string trim [exec {*}$cli ping]]
            assert_equal OK [string trim [exec {*}$cli set affinity-tool-smoke value]]
            assert_equal value [string trim [exec {*}$cli get affinity-tool-smoke]]

            r config resetstat
            set output [exec $::redis_benchmark_path \
                -h [srv host] -p [srv port] -q -n 200 -c 2 -P 4 -t set,get 2>@1]
            assert_match {*SET:*} $output
            assert_match {*GET:*} $output
            assert_match {calls=200,*,rejected_calls=0,failed_calls=0} [cmdrstat set r]
            assert_match {calls=200,*,rejected_calls=0,failed_calls=0} [cmdrstat get r]
        }
    }
}
