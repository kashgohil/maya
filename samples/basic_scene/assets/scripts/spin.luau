-- Turns its entity about a local axis, like the built-in maya.spin component, as a script.
local Spin = {}

Spin.properties = {
    axis = { type = "vector", default = vector.create(0, 1, 0), label = "Axis" },
    speed = { type = "number", default = math.pi / 4, unit = "rad/s", label = "Speed" },
}

function Spin:fixed_update(dt: number)
    if vector.magnitude(self.axis) <= 1e-6 or self.speed == 0 then
        return
    end
    local turn = maya.quaternion.from_axis_angle(self.axis, self.speed * dt)
    self.entity:set_rotation((self.entity:rotation() * turn):normalized())
end

return Spin
