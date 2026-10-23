-- Native character, collision and animation stages are shared with C++ games.
local ticks, shots, hits, retired, failures = 0, 0, 0, 0, 0
local player, last_action
local contacts, landed, jumps, markers = 0, 0, 0, 0
local live = {}
local preparing, pooling, pool

function on_load(scene)
    assert(scene:authored("target"))
    assert(scene:register_system("lua-range.contacts", {
        phase="post_physics", all={"CharacterController3D"}
    }, function(entity, dt, ctx)
        for _, event in ipairs(scene3d.contact_events()) do
            if event.kind == "begin" then contacts = contacts + 1 end
        end
        for _, event in ipairs(scene3d.character_events(player)) do
            if event.kind == "landed" then landed = landed + 1 end
            if event.kind == "left_ground" and event.jumped then jumps = jumps + 1 end
        end
    end))
    assert(scene:register_system("lua-range.animation", {
        phase="audio", all={"Animator3D"}
    }, function(entity, dt, ctx)
        local events, dropped = scene3d.animation_events(player)
        assert(dropped == 0)
        for _, event in ipairs(events) do
            if event.name == "pickup_contact" and event.action and scene3d.action_event_valid(event.action) then
                markers = markers + 1
            end
        end
    end))
end

function on_enter(ctx)
    player = scene3d.entity("player")
    assert(player:alive())
    preparing = scene3d.preload("prefabs/camp-crate.kinprefab")
end

function collect_actions(actions)
    actions:add("launch")
    actions:add("quit")
    actions:add("reload")
    for _, action in ipairs({"forward", "back", "left", "right", "jump", "sprint", "crouch", "action"}) do
        actions:add(action)
    end
end

function update(ctx)
    if ctx.input:pressed("quit") then ctx.app:quit(); return end
    ticks = ticks + 1
    if preparing.status == "failed" then error(preparing.error) end
    if preparing.status == "succeeded" and not pooling then
        pooling = scene3d.create_pool(preparing.prepared, {capacity=8})
    end
    if pooling then
        if pooling.status == "failed" then error(pooling.error) end
        if pooling.status == "succeeded" then pool = pooling.pool end
    end
    local function held(action) return ctx.input:held(action) and 1 or 0 end
    local x, y = held("right") - held("left"), held("forward") - held("back")
    if range_autoplay then x = ticks % 180 < 90 and 0.6 or -0.6 end
    assert(scene3d.character_input(player, {
        move={x=x,y=y}, jump=ctx.input:held("jump") or (range_autoplay and ticks % 180 == 60),
        sprint=ctx.input:held("sprint"), crouch=ctx.input:held("crouch")
    }))
    if ctx.input:pressed("action") or (range_autoplay and ticks % 180 == 121) then
        last_action = scene3d.play_action(player, "pickup") or last_action
    end
    for i = #live, 1, -1 do
        local item = live[i]
        item.time = item.time - ctx.dt
        if item.request.status == "failed" or item.request.status == "cancelled" then
            failures = failures + 1
            table.remove(live, i)
        elseif item.time <= 0 then
            scene3d.despawn(item.request)
            retired = retired + 1
            table.remove(live, i)
        end
    end
    if pool and (ctx.input:pressed("launch") or (range_autoplay and ticks % 90 == 1)) then
        local hit = scene3d.raycast({x=0,y=0.4,z=8}, {x=0,y=0.4,z=-4})
        if hit and hit.entity and scene3d.impulse(hit.entity, {x=1,y=6,z=-2}) then
            hits = hits + 1
        end
        local request = scene3d.spawn(pool,
            {position={x=-2 + shots % 5,y=3,z=0}})
        live[#live+1] = {request=request, time=3}
        shots = shots + 1
    end
end

function write_report()
    local state = scene3d.character_state(player)
    return {steps=ticks, shots=shots, ray_hits=hits, retired=retired, spawn_failures=failures,
        contacts=contacts, landed=landed, jumps=jumps, action_markers=markers,
        action_status=last_action and scene3d.action_status(last_action) or "none",
        player_position=state and state.position, pool=pool and scene3d.pool_stats(pool)}
end
