local bullets, loading = {}, nil
local paused, room, shots, recycled = false, 0, 0, 0
local facing = {x=1,y=0}
function on_load(scene)
    input_contexts.push('play', [[
key fire Space
pad fire a
key pause Escape
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
    loading = preload.scene('rooms/amber.kinscene')
end
local function fire()
    local x,y=game.position()
    local lease=pools.acquire('pulses',x,y)
    if not lease then return end
    local entity=lease:entity()
    tweens.to(entity,'Transform2D.pos',{x=x+facing.x*240,y=y+facing.y*240},.6,'linear')
    bullets[#bullets+1]={lease=lease,entity=entity,age=0}
    shots=shots+1
    game.fired()
    controllers.rumble(1,.15,.3,60)
end
function update(ctx)
    local command=game.command()
    local pause_key=paused and input_contexts.pressed('menu','pause') or input_contexts.pressed('play','pause')
    if command=='pause' or pause_key then
        paused=not paused
        input_contexts.enable('menu',paused)
        game.pause(paused)
    elseif command=='rebind' then
        rebind.begin('fire',8)
        game.status('Press a key or controller button for Fire')
    elseif command=='next' and not loading then
        loading=preload.scene(room%2==1 and 'rooms/azure.kinscene' or 'rooms/amber.kinscene')
    elseif command=='cancel' and loading then
        loading:cancel();loading=nil;game.status('Room loading cancelled')
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
    if loading then
        game.progress(loading:progress())
        if loading:state()=='ready' then
            if loading:commit() then room=room+1 end
            loading=nil
        elseif loading:state()=='failed' then
            game.status(loading:error());loading=nil
        end
    end
    if not paused then
        local x=input_contexts.axis('play','horizontal')
        local y=input_contexts.axis('play','vertical')
        local length=math.sqrt(x*x+y*y)
        if length>1 then x=x/length;y=y/length end
        if length>.1 then local norm=math.sqrt(x*x+y*y);facing={x=x/norm,y=y/norm} end
        game.move(x,y,ctx.dt)
        if command=='fire' or input_contexts.pressed('play','fire') then fire() end
    end
    for i=#bullets,1,-1 do
        local bullet=bullets[i]
        bullet.age=bullet.age+ctx.dt
        local hit=false
        if bullet.lease:alive() then
            local p=bullet.entity:get('Transform2D')
            hit=game.hit(p.x,p.y)
        end
        if hit or bullet.age>=.6 or not bullet.lease:alive() then
            bullet.lease:release()
            assert(not bullet.lease:alive())
            recycled=recycled+1
            table.remove(bullets,i)
        end
    end
    game.counters(shots,recycled,pools.active('pulses'),room)
end
