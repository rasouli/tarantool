#!/usr/bin/env tarantool
repl_list = os.getenv("MASTER")

-- Start the console first to allow test-run to attach even before
-- box.cfg is finished.
require('console').listen(os.getenv('ADMIN'))

box.cfg({
    listen              = os.getenv("LISTEN"),
    replication         = repl_list,
    memtx_memory        = 107374182,
    replication_timeout = 1000,
})
