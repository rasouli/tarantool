test_run = require('test_run').new()
--
-- gh-5288: transaction limbo could crash during recovery, because in WAL write
-- completion callback it woken up the currently active fiber.
--
s = box.schema.space.create('sync', {is_sync = true})
_ = s:create_index('pk')
s:insert{1}
box.snapshot()
test_run:cmd('restart server default with signal=KILL')
box.space.sync:drop()
