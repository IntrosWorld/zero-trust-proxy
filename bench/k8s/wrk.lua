local file = assert(io.open('/run/secrets/proxy-token', 'r'))
local token = file:read('*l')
file:close()
wrk.method = 'GET'
wrk.path = 'http://blocked.invalid/'
wrk.headers['Host'] = 'blocked.invalid'
wrk.headers['Proxy-Authorization'] = 'Bearer ' .. token
wrk.headers['Connection'] = 'close'
