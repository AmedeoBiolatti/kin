local rules = {}
function rules.damage(base, armor)
    assert(base >= 0 and armor >= 0, 'damage inputs must be nonnegative')
    return math.max(0, base - armor)
end
return rules
