test_run = require('test_run').new()
engine = test_run:get_cfg('engine')

box.schema.user.grant('guest', 'replication')

box.cfg { replication_synchro_quorum = "n/2+1", replication_synchro_timeout = 1000 }

test_run:cmd('create server replica1 with rpl_master=default,\
              script="replication/replica-quorum-1.lua"')
test_run:cmd('start server replica1 with wait=True, wait_load=True')

test_run:cmd('create server replica2 with rpl_master=default,\
              script="replication/replica-quorum-2.lua"')
test_run:cmd('start server replica2 with wait=True, wait_load=True')

test_run:cmd('create server replica3 with rpl_master=default,\
              script="replication/replica-quorum-3.lua"')
test_run:cmd('start server replica3 with wait=True, wait_load=True')

test_run:cmd('create server replica4 with rpl_master=default,\
              script="replication/replica-quorum-4.lua"')
test_run:cmd('start server replica4 with wait=True, wait_load=True')

_ = box.schema.space.create('sync', {is_sync = true, engine = engine})
s = box.space.sync

s:format({{name = 'id', type = 'unsigned'}, {name = 'band_name', type = 'string'}})

_ = s:create_index('primary', {parts = {'id'}})
s:insert({1, '1'})
s:insert({2, '2'})
s:insert({3, '3'})

-- Wait the data to be propagated, there is no much
-- need for this since we're testing the quorum number
-- update only but just in case.
test_run:wait_lsn('replica1', 'default')
test_run:wait_lsn('replica2', 'default')
test_run:wait_lsn('replica3', 'default')
test_run:wait_lsn('replica4', 'default')

-- Make sure we were configured in a proper way
match = 'set \'replication_synchro_quorum\' configuration option to \"n\\/2%+1'
test_run:grep_log("default", match) ~= nil

-- There are 4 replicas -> replication_synchro_quorum = 4/2 + 1 = 3
match = 'renew replication_synchro_quorum = 3'
test_run:grep_log("default", match) ~= nil

test_run:cmd('stop server replica1')
test_run:cmd('delete server replica1')

test_run:cmd('stop server replica2')
test_run:cmd('delete server replica2')

test_run:cmd('stop server replica3')
test_run:cmd('delete server replica3')

test_run:cmd('stop server replica4')
test_run:cmd('delete server replica4')

box.schema.user.revoke('guest', 'replication')
