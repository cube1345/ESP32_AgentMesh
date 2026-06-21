local ea = require("espagent")
local label = args and args.label or "default"

print("lua_smoke label=" .. tostring(label))

local ok, out, err = ea.call_capability("get_current_time", "{}")

if ok then
    return "lua_smoke ok: " .. out
end

return "lua_smoke failed: err=" .. tostring(err) .. " output=" .. tostring(out)
