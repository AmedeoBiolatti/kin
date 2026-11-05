local rules = require('rules')

test('rules and errors', function()
    t.equal(rules.damage(12, 5), 7)
    t.equal(rules.damage(2, 5), 0)
    t.raises(function() rules.damage(-1, 0) end, 'nonnegative')
end)

test('controlled input, time and entity tasks', function()
    local hero = t.spawn('Hero', {Transform2D = {}})
    t.attach(hero, 'actor.lua')
    t.input('right', true)
    t.step(2, 0.1)
    t.near(t.get(hero, 'Transform2D').pos.x, 20)
    t.input('right', false)
    t.step(1, 0.1)
    t.near(t.get(hero, 'Transform2D').pos.x, 25)
    t.near(t.time(), 0.3)
    t.equal(t.frame(), 3)
    assert(t.reload('actor.lua'))
    t.input('right', true)
    t.step(1, 0.1)
    t.near(t.get(hero, 'Transform2D').pos.x, 45)
end)

test('rejected edit retains live script', function()
    local hero = t.spawn('Hero', {Transform2D = {}})
    t.attach(hero, 'actor.lua')
    t.input('right', true)
    t.step(1, 0.1)
    t.edit('actor.lua', 'function on_reload(self) error("rejected") end')
    assert(not t.reload('actor.lua'))
    assert(t.errors()['actor.lua']:find('rejected', 1, true))
    t.step(1, 0.1)
    t.near(t.get(hero, 'Transform2D').pos.x, 20)
end)

test('module task waits use simulation time', function()
    local done = false
    local job = task.run(function() task.frames(2); done = true end)
    t.step(2)
    assert(not done)
    t.step(1)
    assert(done and job:state() == 'completed')
end)

test('seed and fixture isolation', function()
    t.equal(t.time(), 0)
    t.seed(123)
    local first = t.random()
    t.seed(123)
    t.equal(t.random(), first)
end)

test('scene state survives reload', function()
    local counter = t.spawn('Counter', {Transform2D = {}})
    t.edit('scene.lua', [[
        reload_state = {score = 1.25}
        function on_load(scene)
            local counter = scene:find('Counter')
            local tween = tweens.to(counter, 'Transform2D.pos', {x=10,y=0}, 0.2)
            task.run(function() task.await(tween); counter:translate(5,0) end)
        end
        function update(ctx) reload_state.score = reload_state.score + 1 end
        function on_reload(saved, scene) reload_state = saved end
    ]])
    t.scene('scene.lua')
    t.step(2, 0.1)
    t.near(t.get(counter, 'Transform2D').pos.x, 15)
    t.edit('scene.lua', [[
        function on_reload(saved, scene)
            assert(saved.score == 3.25)
            reload_state = saved
        end
        function update(ctx) assert(reload_state.score == 3.25) end
    ]])
    assert(t.reload_scene('scene.lua'), t.reload_error())
    t.step(1)
end)
