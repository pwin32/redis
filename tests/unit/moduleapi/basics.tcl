set testmodule [redis_test_module basics]


start_server {tags {"modules"}} {
    r module load $testmodule

    test {test module api basics} {
        r test.basics
    } {ALL TESTS PASSED}

    r module unload test
}
