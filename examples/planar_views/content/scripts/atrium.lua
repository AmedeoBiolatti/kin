-- The atrium's motion: the brass ring turning over the pool, the orb flying through the portals, and the camera.
-- Things go through a portal by scene3d.portal_step: a step that goes into one comes out of the other, and its
-- velocity and facing turn as the portals turn it.
--
-- The host sets `atrium_camera` (planar_atrium --camera): 6 plays (the survivor is yours: the host binds "forward",
-- "back", "left", "right" and "run"; seen from behind, the camera following it through the portals), 0 orbits the
-- atrium, 1 looks at the mirror, 2 at the pool, 3 into the west portal, 4 walks through the portals, round and round,
-- 5 watches the survivor walk its round into the west portal (and out of the east one, in its picture).
--
-- The survivor (an animated character: its walk) goes round the pool's south side and into the west portal, out of
-- the east one, and round again. Its steps go through a portal at its waist (the openings start above the floor).

local world
local time = 0
local orb = {position = {x = 6, y = 1.0, z = 2.7}, velocity = {x = -3, y = 0, z = 0}}
local walker = {position = {x = -2, y = 1.75, z = 2.3}, velocity = {x = -1.6, y = 0, z = 0},
                facing = {x = 0, y = 0.7071068, z = 0, w = 0.7071068}} -- looking west (-X)
-- The survivor's way (x, z): from the east portal's front round the pool's south side to the west portal's, then on
-- west, into it.
local survivor = {position = {x = 9.0, y = 0.0, z = 2.0}, next = 1, speed = 1.3, yaw = -90, clip = "Walk"}
local route = {{x = 8.0, z = 7.0}, {x = -8.0, z = 7.0}, {x = -9.0, z = 2.0}, {x = -14.0, z = 2.0}}

local function quat(x, y, z, degrees)
    local half = math.rad(degrees) / 2
    local s = math.sin(half)
    return {x = x * s, y = y * s, z = z * s, w = math.cos(half)}
end
local function mul(a, b)
    return {w = a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
            x = a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            y = a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            z = a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w}
end
local function rotate(q, v) -- v turned by the unit quaternion q
    local tx, ty, tz = 2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x)
    return {x = v.x + q.w * tx + (q.y * tz - q.z * ty),
            y = v.y + q.w * ty + (q.z * tx - q.x * tz),
            z = v.z + q.w * tz + (q.x * ty - q.y * tx)}
end
local function normalized(q)
    local n = math.sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w)
    return {x = q.x / n, y = q.y / n, z = q.z / n, w = q.w / n}
end
-- A camera's rotation looking from `from` at `to` (it looks down its -Z, +Y up): turned, then tilted.
local function look(from, to)
    local dx, dy, dz = to.x - from.x, to.y - from.y, to.z - from.z
    local yaw = math.deg(math.atan(-dx, -dz))
    local pitch = math.deg(math.atan(dy, math.sqrt(dx * dx + dz * dz)))
    return normalized(mul(quat(0, 1, 0, yaw), quat(1, 0, 0, pitch)))
end
-- One step of a mover: through a portal when it crosses one.
local function fly(mover, dt)
    local p, v = mover.position, mover.velocity
    local to = {x = p.x + v.x * dt, y = p.y + v.y * dt, z = p.z + v.z * dt}
    local through = scene3d.portal_step(p, to)
    if through then
        mover.position = through.position
        mover.velocity = rotate(through.turn, v)
        if mover.facing then mover.facing = normalized(mul(through.turn, mover.facing)) end
    else
        mover.position = to
    end
end

function on_load(scene)
    world = scene
end

-- A step of the survivor toward its next point; through a portal at its waist (out of the exit, its way turned). Out of
-- the east portal it starts its round again.
local function walk(dt)
    local p, goal = survivor.position, route[survivor.next]
    local dx, dz = goal.x - p.x, goal.z - p.z
    local d = math.sqrt(dx * dx + dz * dz)
    if d < 0.05 then
        survivor.next = math.min(survivor.next + 1, #route)
        return
    end
    local s = math.min(survivor.speed * dt, d)
    local vx, vz = dx / d, dz / d
    local waist = 1.0
    local from = {x = p.x, y = p.y + waist, z = p.z}
    local to = {x = p.x + vx * s, y = p.y + waist, z = p.z + vz * s}
    local through = scene3d.portal_step(from, to)
    if through then
        survivor.position = {x = through.position.x, y = through.position.y - waist, z = through.position.z}
        survivor.next = 1
    else
        survivor.position = {x = to.x, y = p.y, z = to.z}
    end
    -- Facing its way (the model looks down +Z).
    survivor.yaw = math.deg(math.atan(vx, vz))
    survivor.facing = quat(0, 1, 0, survivor.yaw)
end

-- What the survivor cannot walk through (x0, z0, x1, z1): the pool's rim, the plinths, the columns' bases; and the
-- atrium's walls, but for the portals' openings (z 1 to 3), where it walks on through.
local radius = 0.35
local blocks = {{-4.45, -0.9, 4.45, 4.9}, {-7.05, 4.95, -5.95, 6.05}, {5.95, 4.95, 7.05, 6.05},
                {-6.8, -6.1, -5.6, -4.9}}
for _, c in ipairs({{-8, -8.4}, {-4.5, -8.4}, {4.5, -8.4}, {8, -8.4}, {-9.6, -2.5}, {-9.6, 6.5}, {9.6, -2.5},
                    {9.6, 6.5}}) do
    blocks[#blocks + 1] = {c[1] - 0.65, c[2] - 0.65, c[1] + 0.65, c[2] + 0.65}
end
local function keep_out(p)
    local through_portal = math.abs(p.z - 2.0) < 0.75
    local wall = through_portal and 12.5 or 11.0
    p.x = math.max(-wall, math.min(wall, p.x))
    p.z = math.max(-9.5, math.min(10.6, p.z))
    for _, b in ipairs(blocks) do
        local x0, z0, x1, z1 = b[1] - radius, b[2] - radius, b[3] + radius, b[4] + radius
        if p.x > x0 and p.x < x1 and p.z > z0 and p.z < z1 then
            -- Out by the nearest side.
            local left, right, near, far = p.x - x0, x1 - p.x, p.z - z0, z1 - p.z
            local least = math.min(left, right, near, far)
            if least == left then p.x = x0 elseif least == right then p.x = x1
            elseif least == near then p.z = z0 else p.z = z1 end
        end
    end
end

-- The player's step: turning by left and right, walking (running with "run") by forward and back; through a portal at
-- its waist, its yaw turned as the portals turn it. Its animation follows: idle, walking or running.
local function play(ctx, dt)
    local input = ctx.input
    local turn = (input:held("left") and 1 or 0) - (input:held("right") and 1 or 0)
    local ahead = (input:held("forward") and 1 or 0) - (input:held("back") and 1 or 0)
    local running = input:held("run") and ahead > 0
    survivor.yaw = survivor.yaw + turn * 150 * dt
    local speed = ahead * (running and 4.0 or 1.6)
    local yaw = math.rad(survivor.yaw)
    local vx, vz = math.sin(yaw) * speed, math.cos(yaw) * speed
    local p = survivor.position
    local waist = 1.0
    local from = {x = p.x, y = p.y + waist, z = p.z}
    local to = {x = p.x + vx * dt, y = p.y + waist, z = p.z + vz * dt}
    local through = scene3d.portal_step(from, to)
    if through then
        survivor.position = {x = through.position.x, y = through.position.y - waist, z = through.position.z}
        -- The yaw turned as the portal turns its way.
        local way = rotate(through.turn, {x = math.sin(yaw), y = 0, z = math.cos(yaw)})
        survivor.yaw = math.deg(math.atan(way.x, way.z))
    else
        survivor.position = {x = to.x, y = p.y, z = to.z}
        keep_out(survivor.position)
    end
    survivor.facing = quat(0, 1, 0, survivor.yaw)
    local clip = speed == 0 and "Idle" or (running and "Run" or "Walk")
    if clip ~= survivor.clip then
        survivor.clip = clip
        world:authored("survivor"):patch("Animator3D", {clip = clip})
    end
end

-- The camera behind the survivor, a little above, looking at its head. Where the line from its head back to the camera
-- goes into a portal (the survivor just came out of one), the camera is where that line comes out, turned with it: it
-- sees the survivor through the other portal, as one would.
local function follow()
    local p = survivor.position
    local yaw = math.rad(survivor.yaw)
    local head = {x = p.x, y = p.y + 1.6, z = p.z}
    local back = 3.2
    local eye = {x = head.x - math.sin(yaw) * back, y = head.y + 0.7, z = head.z - math.cos(yaw) * back}
    local looking = look(eye, {x = head.x, y = head.y - 0.2, z = head.z})
    local through = scene3d.portal_step(head, eye)
    if through then
        return through.position, normalized(mul(through.turn, looking))
    end
    return eye, looking
end

function update(ctx)
    local dt = ctx.dt
    time = time + dt
    fly(orb, dt)
    fly(walker, dt)
    local mode = atrium_camera or 0
    if mode == 6 then
        play(ctx, dt)
    else
        walk(dt)
        if survivor.clip ~= "Walk" then
            survivor.clip = "Walk"
            world:authored("survivor"):patch("Animator3D", {clip = "Walk"})
        end
    end
    world:authored("survivor"):patch("Transform3D", {position = survivor.position, rotation = survivor.facing})
    world:authored("orb"):patch("Transform3D", {position = orb.position})
    world:authored("ring"):patch("Transform3D", {rotation = normalized(mul(quat(0, 1, 0, time * 25), quat(1, 0, 0, 68)))})

    local position, target
    if mode == 1 then
        position, target = {x = 3.2, y = 2.4, z = 3.0}, {x = -0.4, y = 2.2, z = -10}
    elseif mode == 2 then
        position, target = {x = -3.5, y = 1.35, z = 8.0}, {x = 0.5, y = 0.6, z = 0}
    elseif mode == 3 then
        position, target = {x = -4.5, y = 1.8, z = 6.0}, {x = -11.45, y = 1.6, z = 2.0}
    elseif mode == 5 then
        position, target = {x = -6.0, y = 1.7, z = 5.5}, {x = -11.45, y = 1.1, z = 2.0}
    end
    local rotation
    if mode == 6 then
        position, rotation = follow()
    elseif mode == 4 then
        position, rotation = walker.position, walker.facing
    elseif position then
        rotation = look(position, target)
    else
        local angle = 0.35 + time * 0.06
        position = {x = math.sin(angle) * 15, y = 6.2, z = 2 + math.cos(angle) * 15}
        rotation = look(position, {x = 0, y = 1.4, z = 0})
    end
    world:authored("camera"):patch("Transform3D", {position = position, rotation = rotation})
end
