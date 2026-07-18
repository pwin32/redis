proc rediscli_tls_config {testsdir} {
    set tlsdir [file join $testsdir tls]
    set cert [file join $tlsdir client.crt]
    set key [file join $tlsdir client.key]
    set cacert [file join $tlsdir ca.crt]

    if {$::tls} {
        return [list --tls --cert $cert --key $key --cacert $cacert]
    } else {
        return {}
    }
}

# Returns command line for executing redis-cli
proc rediscli {host port {opts {}}} {
    set cmd [list $::redis_cli_path -h $host -p $port]
    lappend cmd {*}[rediscli_tls_config "tests"]
    lappend cmd {*}$opts
    return $cmd
}

# Returns command line for executing redis-cli with a unix socket address
proc rediscli_unixsocket {unixsocket {opts {}}} {
    if {$::tcl_platform(platform) eq "windows"} {
        error "Unix-domain sockets are not supported by the Windows port"
    }
    return [list $::redis_cli_path -s $unixsocket {*}$opts]
}

# Run redis-cli with specified args on the server of specified level.
# Returns output broken down into individual lines.
proc rediscli_exec {level args} {
    if {$::tcl_platform(platform) eq "windows"} {
        # Keep the command-level coverage while using the supported TCP
        # listener.  The Windows port intentionally does not expose Unix
        # sockets, so there is no socket pathname to pass to redis-cli.
        set cmd [rediscli [srv $level host] [srv $level port] $args]
    } else {
        set cmd [rediscli_unixsocket [srv $level unixsocket] $args]
    }
    set fd [open "|$cmd" "r"]
    set ret [lrange [split [read $fd] "\n"] 0 end-1]
    close $fd

    return $ret
}
