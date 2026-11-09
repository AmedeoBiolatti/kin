-- The atrium's motion: the brass ring turning over the pool, the orb flying through the portals, the survivor, and the
-- camera. Things go through a portal by scene3d.portal_step (the orb, the walking camera) or, with a body, by physics
-- (the survivor: a CharacterController3D with a PortalTraveller3D, carried through with its velocity turned).
--
-- The host sets `atrium_camera` (planar_atrium --camera): 6 plays (the survivor is yours: the host binds "forward",
-- "back", "left", "right", "jump" and "sprint", and gives the mouse's motion in `atrium_look_x`, `atrium_look_y`;
-- seen from behind, the camera orbiting it with the mouse and following it through the portals), 0 orbits the atrium,
-- 1 looks at the mirror, 2 at the pool, 3 into the west portal, 4 walks through the portals, round and round, 5 watches
-- the survivor walk its round (by itself, through the same physics) into the west portal.

local world
local time = 0
local orb = {position = {x = 6, y = 1.0, z = 2.7}, velocity = {x = -3, y = 0, z = 0}}
local walker = {position = {x = -2, y = 1.75, z = 2.3}, velocity = {x = -1.6, y = 0, z = 0},
                facing = {x = 0, y = 0.7071068, z = 0, w = 0.7071068}} -- looking west (-X)
-- The survivor: the way it faces (degrees about +Y; the model looks down +Z), its place the step before (to see it go
-- through a portal), the point of its round it walks to next and where it starts (and starts again if it ever falls);
-- its round when it walks by itself (x, z: south of the pool, into the west portal; out of the east one it starts
-- again).
local survivor = {face = 0, last = nil, next = 1, spawn = {x = 6.0, y = 0.05, z = 7.5}}
local route = {{x = 6.0, z = 7.5}, {x = -8.0, z = 7.5}, {x = -9.5, z = 2.0}, {x = -14.0, z = 2.0}}
-- The play camera: its yaw (degrees: it looks along (sin, cos)) and pitch round the survivor, and how far back.
local cam = {yaw = 180, pitch = -14, distance = 3.4}

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

local function horizontal(yaw)
    local r = math.rad(yaw)
    return {x = math.sin(r), y = 0, z = math.cos(r)}
end
local function yaw_of(v)
    return math.deg(math.atan(v.x, v.z))
end
local function wrap(degrees)
    return (degrees + 180) % 360 - 180
end
-- The survivor's yaw from its rotation (the way its +Z faces).
local function rotation_yaw(q)
    return yaw_of(rotate(q, {x = 0, y = 0, z = 1}))
end

-- What the survivor's controller is told this step: `move` (x right, y ahead) in the frame of `forward`, and turning
-- its facing toward the way it goes (at most 540 degrees a second).
local function command(dt, move, forward, jump, sprint)
    local player = scene3d.entity("survivor")
    if not player then return end
    if move.x ~= 0 or move.y ~= 0 then
        local right = {x = -forward.z, y = 0, z = forward.x}
        local way = {x = forward.x * move.y + right.x * move.x, y = 0, z = forward.z * move.y + right.z * move.x}
        local want = yaw_of(way)
        local turn = wrap(want - survivor.face)
        local most = 540 * dt
        survivor.face = survivor.face + math.max(-most, math.min(most, turn))
    end
    scene3d.character_input(player, {move = move, forward = forward, jump = jump, sprint = sprint,
                                     facing = quat(0, 1, 0, survivor.face)})
end

-- Where the survivor is (its feet), and how far physics turned it since the step before (degrees): its rotation is the
-- facing it was given unless a portal carried it through, which turns it (the facing and the play camera turn with it).
-- Also whether it went through (a jump in its place: its round starts again).
local function where()
    local player = scene3d.entity("survivor")
    local state = player and scene3d.character_state(player)
    local transform = world:authored("survivor"):get("Transform3D")
    if not state or not transform then return nil, 0, false end
    local p, yaw = state.position, rotation_yaw(transform.rotation)
    -- Fallen off somewhere (there should be nowhere): back where it started (moving its Transform3D teleports it).
    if p.y < -4 then
        world:authored("survivor"):patch("Transform3D", {position = survivor.spawn})
        survivor.last = nil
        return nil, 0, false
    end
    local turn, through = 0, false
    if survivor.last then
        turn = wrap(yaw - survivor.face)
        if math.abs(turn) < 0.5 then turn = 0 end
        local dx, dz = p.x - survivor.last.x, p.z - survivor.last.z
        through = dx * dx + dz * dz > 4
    else
        survivor.face = yaw
    end
    survivor.last = {x = p.x, y = p.y, z = p.z}
    return p, turn, through
end

-- The survivor's round by itself: toward its next point, walking; out of the east portal it starts again.
local function autopilot(dt, p, through)
    if through then survivor.next = 1 end
    local goal = route[survivor.next]
    local dx, dz = goal.x - p.x, goal.z - p.z
    if dx * dx + dz * dz < 0.25 then
        survivor.next = math.min(survivor.next + 1, #route)
        goal = route[survivor.next]
        dx, dz = goal.x - p.x, goal.z - p.z
    end
    local d = math.sqrt(dx * dx + dz * dz)
    command(dt, {x = 0, y = 1}, {x = dx / d, y = 0, z = dz / d}, false, false)
end

-- The player's step: moving in the camera's frame (WASD, the arrows), jumping, sprinting; the mouse turns the camera.
local function play(ctx, dt)
    local input = ctx.input
    local function held(action) return input:held(action) and 1 or 0 end
    cam.yaw = cam.yaw - (atrium_look_x or 0) * 0.15
    cam.pitch = math.max(-70, math.min(35, cam.pitch - (atrium_look_y or 0) * 0.15))
    atrium_look_x, atrium_look_y = 0, 0
    local move = {x = held("right") - held("left"), y = held("forward") - held("back")}
    command(dt, move, horizontal(cam.yaw), input:held("jump"), input:held("sprint"))
end

-- The play camera: behind the survivor's head along the camera's yaw and pitch, pulled in where something is in the
-- way (a ray from the head); where the line back from the head goes into a portal, it is where the line comes out,
-- turned with it (it sees the survivor through the other portal, as one would).
local function follow(p)
    local head = {x = p.x, y = p.y + 1.55, z = p.z}
    local yaw, pitch = math.rad(cam.yaw), math.rad(cam.pitch)
    local back = {x = -math.sin(yaw) * math.cos(pitch), y = -math.sin(pitch), z = -math.cos(yaw) * math.cos(pitch)}
    local eye = {x = head.x + back.x * cam.distance, y = head.y + back.y * cam.distance,
                 z = head.z + back.z * cam.distance}
    local looking = look(eye, head)
    local through = scene3d.portal_step(head, eye)
    if through then
        return through.position, normalized(mul(through.turn, looking))
    end
    -- (From just outside the survivor's capsule, so the ray does not find it.)
    local from = {x = head.x + back.x * 0.35, y = head.y + back.y * 0.35, z = head.z + back.z * 0.35}
    local hit = scene3d.raycast(from, eye)
    if hit and hit.position then
        local hx, hy, hz = hit.position.x - head.x, hit.position.y - head.y, hit.position.z - head.z
        local reach = math.max(0.4, math.sqrt(hx * hx + hy * hy + hz * hz) - 0.2)
        eye = {x = head.x + back.x * reach, y = head.y + back.y * reach, z = head.z + back.z * reach}
    end
    return eye, looking
end

function update(ctx)
    local dt = ctx.dt
    time = time + dt
    fly(orb, dt)
    fly(walker, dt)
    world:authored("orb"):patch("Transform3D", {position = orb.position})
    world:authored("ring"):patch("Transform3D", {rotation = normalized(mul(quat(0, 1, 0, time * 25), quat(1, 0, 0, 68)))})

    local mode = atrium_camera or 0
    local p, turned, through = where()
    if p then
        survivor.face = survivor.face + turned
        if mode == 6 then
            cam.yaw = cam.yaw + turned
            play(ctx, dt)
        else
            autopilot(dt, p, through)
        end
    end

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
    if mode == 6 and p then
        position, rotation = follow(p)
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
