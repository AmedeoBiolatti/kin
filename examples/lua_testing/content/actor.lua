function on_start(self)
    self.steps = 0
    self.speed = 100
    self.reward = task.run(function()
        task.wait(0.2)
        self:move(5, 0)
    end)
end

function on_update(self, dt)
    self.steps = self.steps + 1
    if input.held('right') then self:move(self.speed * dt, 0) end
end

function on_reload(self)
    -- Tasks have an explicit cancellation boundary at reload.
    assert(self.reward:state() == 'cancelled')
    self.speed = 200
end
