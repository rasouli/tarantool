env = require('test_run')
test_run = env.new()
socket = require('socket')

-- gh-4138 Check getaddrinfo() error from socket:connect() only.
-- Error code and error message returned by getaddrinfo() depends
-- on system's gai_strerror().
test_run:cmd("setopt delimiter ';'")
function check_err(err)
-- EAI_NONAME
    if err == 'getaddrinfo: nodename nor servname provided, or not known' or
-- EAI_SERVICE
       err == 'getaddrinfo: Servname not supported for ai_socktype' or
-- EAI_NONAME
       err == 'getaddrinfo: Name or service not known' or
-- EAI_NONAME
       err == 'getaddrinfo: hostname nor servname provided, or not known' or
-- EAI_AGAIN
       err == 'getaddrinfo: Temporary failure in name resolution' or
-- EAI_AGAIN
       err == 'getaddrinfo: Name could not be resolved at this time' or
-- EAI_NONAME
       err == 'getaddrinfo: No address associated with hostname' then
        return true
    end
    return false
end;
test_run:cmd("setopt delimiter ''");

s, err = socket.getaddrinfo('non_exists_hostname', 3301)
check_err(err)
s, err = socket.connect('non_exists_hostname', 3301)
check_err(err)
s, err = socket.tcp_connect('non_exists_hostname', 3301)
check_err(err)
s, err = socket.bind('non_exists_hostname', 3301)
check_err(err)
s, err = socket.tcp_server('non_exists_hostname', 3301, function() end)
check_err(err)
