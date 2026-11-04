local MOVE_SCALE = 1
reload_state = {version=1, bullets={}, paused=false, room=0, shots=0, recycled=0, facing={x=1,y=0}}
local state = reload_state
function on_reload(saved)
    assert(type(saved)=='table' and saved.version==1, 'unsupported Workshop reload state')
    state=saved
    reload_state=saved
end
function on_load(scene)
    input_contexts.push('play', [[
key fire Space
pad fire a
key pause Escape
key next_room N
axis horizontal - key:A
axis horizontal + key:D
axis horizontal stick left_x
axis vertical - key:W
axis vertical + key:S
axis vertical stick -left_y
]])
    pools.create('pulses','pulse.kinprefab',32)
    input_contexts.push('menu','key pause Escape\npad pause start\n',true)
    input_contexts.enable('menu',false)
    state.loading = preload.scene('rooms/amber.kinscene')
end
local function fire()
    local x,y=game.position()
    local lease=pools.acquire('pulses',x,y)
    if not lease then return end
    local entity=lease:entity()
    tweens.to(entity,'Transform2D.pos',{x=x+state.facing.x*240,y=y+state.facing.y*240},.6,'linear')
    state.bullets[#state.bullets+1]={lease=lease,age=0}
    state.shots=state.shots+1
    game.fired()
    controllers.rumble(1,.15,.3,60)
end
function update(ctx)
    local command=game.command()
    -- The contexts are the one source: the game mirrors 'play' into its Input map,
    -- so automation's input.action presses what they read.
    if command=='' and input_contexts.pressed('play','next_room') then command='next' end
    local pause_key=input_contexts.pressed(state.paused and 'menu' or 'play','pause')
    if command=='pause' or pause_key then
        state.paused=not state.paused
        input_contexts.enable('menu',state.paused)
        game.pause(state.paused)
    elseif command=='rebind' then
        rebind.begin('fire',8)
        game.status('Press a key or controller button for Fire')
    elseif command=='next' and not state.loading then
        state.loading=preload.scene(state.room%2==1 and 'rooms/azure.kinscene' or 'rooms/amber.kinscene')
    elseif command=='cancel' and state.loading then
        state.loading:cancel();state.loading=nil;game.status('Room loading cancelled')
    elseif command=='trim' then
        asset_memory.budget(1024,1024)
        asset_memory.trim()
        game.room_budget(1024)
        game.status('Live assets retained; unused cache entries evicted')
    end
    if rebind.state()=='captured' then
        rebind.apply('play','replace')
        input_contexts.save('play','workshop')
        game.status('Fire binding saved')
        rebind.cancel()
    end
    if state.loading then
        game.progress(state.loading:progress())
        if state.loading:state()=='ready' then
            if state.loading:commit() then state.room=state.room+1 end
            state.loading=nil
        elseif state.loading:state()=='failed' then
            game.status(state.loading:error());state.loading=nil
        end
    end
    if not state.paused then
        local x=input_contexts.axis('play','horizontal')
        local y=input_contexts.axis('play','vertical')
        local length=math.sqrt(x*x+y*y)
        if length>1 then x=x/length;y=y/length end
        if length>.1 then local norm=math.sqrt(x*x+y*y);state.facing={x=x/norm,y=y/norm} end
        game.move(x*MOVE_SCALE,y*MOVE_SCALE,ctx.dt)
        if command=='fire' or input_contexts.pressed('play','fire') then fire() end
    end
    for i=#state.bullets,1,-1 do
        local bullet=state.bullets[i]
        bullet.age=bullet.age+ctx.dt
        local hit=false
        if bullet.lease:alive() then
            local p=bullet.lease:entity():get('Transform2D')
            hit=game.hit(p.x,p.y)
        end
        if hit or bullet.age>=.6 or not bullet.lease:alive() then
            bullet.lease:release()
            assert(not bullet.lease:alive())
            state.recycled=state.recycled+1
            table.remove(state.bullets,i)
        end
    end
    game.counters(state.shots,state.recycled,pools.active('pulses'),state.room)
end
