-- The atrium's motion: the brass ring turning over the pool, the orb flying through the portals, and the camera.
-- Things go through a portal by scene3d.portal_step: a step that goes into one comes out of the other, and its
-- velocity and facing turn as the portals turn it.
--
-- The host sets `atrium_camera` (planar_views --scene): 0 orbits the atrium, 1 looks at the mirror, 2 at the pool,
-- 3 into the west portal, 4 walks through the portals, round and round.

local world
local time = 0
local orb = {position = {x = 6, y = 1.0, z = 2.7}, velocity = {x = -3, y = 0, z = 0}}
local walker = {position = {x = -2, y = 1.75, z = 2.3}, velocity = {x = -1.6, y = 0, z = 0},
                facing = {x = 0, y = 0.7071068, z = 0, w = 0.7071068}} -- looking west (-X)

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

function update(ctx)
    local dt = ctx.dt
    time = time + dt
    fly(orb, dt)
    fly(walker, dt)
    world:authored("orb"):patch("Transform3D", {position = orb.position})
    world:authored("ring"):patch("Transform3D", {rotation = normalized(mul(quat(0, 1, 0, time * 25), quat(1, 0, 0, 68)))})

    local mode = atrium_camera or 0
    local position, target
    if mode == 1 then
        position, target = {x = 3.2, y = 2.4, z = 3.0}, {x = -0.4, y = 2.2, z = -10}
    elseif mode == 2 then
        position, target = {x = -3.5, y = 1.35, z = 8.0}, {x = 0.5, y = 0.6, z = 0}
    elseif mode == 3 then
        position, target = {x = -4.5, y = 1.8, z = 6.0}, {x = -11.45, y = 1.6, z = 2.0}
    end
    local rotation
    if mode == 4 then
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
