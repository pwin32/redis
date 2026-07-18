start_server {tags {"limits network external:skip"} overrides {maxclients 10}} {
    if {$::tls} {
        set expected_code "*I/O error*"
    } else {
        set expected_code "*ERR max*reached*"
    }
    test {Check if maxclients works refusing connections} {
        set c 0
        catch {
            while {$c < 50} {
                incr c
                set rd [redis_deferring_client]
                $rd ping
                $rd read
                after 100
            }
        } e
        assert {$c > 8 && $c <= 10}
        set e
    } $expected_code
}

if {$::tcl_platform(platform) eq "windows" && !$::tls} {
    start_server {tags {"limits network"} overrides {maxclients 10}} {
        test {Maxclients rejection burst keeps the server responsive} {
            set fillers {}
            set available [expr {10-[s connected_clients]}]
            for {set i 0} {$i < $available} {incr i} {
                set rd [redis [srv 0 host] [srv 0 port] 1 $::tls]
                $rd ping
                assert_equal PONG [$rd read]
                lappend fillers $rd
            }
            assert_equal 10 [s connected_clients]

            set rejected {}
            set start [clock milliseconds]
            for {set i 0} {$i < 100} {incr i} {
                set rd [redis [srv 0 host] [srv 0 port] 1 $::tls]
                $rd ping
                lappend rejected $rd
            }

            foreach rd $rejected {
                assert {[catch {$rd read} err] == 1}
                assert_match {*ERR max*reached*} $err
                $rd close
            }
            assert_equal PONG [r ping]
            assert {[clock milliseconds]-$start < 5000}

            foreach rd $fillers {
                $rd close
            }
        }
    }
}
