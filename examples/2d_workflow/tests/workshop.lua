scenario {
    name = 'Workshop Lua scenario', seed = 7,
    requires = {'ui', 'world', 'reset'},
    dependencies = {'../content', '../art'},
    steps = {
        wait_until {observe='scene.current', path='/state/rooms', value=1, within=16},
        call('ui.action', {selector={key='fire', role='button', within={key='control_room'}}}),
        wait_until {observe='scene.current', path='/state/shots', value=1, within=3},
        wait_until {observe='scene.current', path='/state/active', value=0, within=40},
        expect {observe='scene.current', path='/state/recycled', value=1},
        call('input.action', {name='pause'}),
        wait_until {observe='scene.current', path='/state/paused', value=true, within=2},
        always {observe='scene.current', path='/state/paused', value=true, ticks=3},
        call('input.action', {name='pause'}),
        wait_until {observe='scene.current', path='/state/paused', value=false, within=2},
    }
}
