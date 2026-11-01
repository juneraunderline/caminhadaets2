# Changes

## 0.19-dev (prototype)

- Show a pair of forearms and open hands as a transparent 2D overlay while walking.
- Press E to inspect the nearest solid collision object up to 4.5 m ahead; the object description is written to the game log. This is diagnostic groundwork, not yet an interaction action.
- The 0.18 stable branch is unchanged. This prototype is not yet validated in-game or against ETS2 1.61.1.9.

## 0.18

- **Fixed: F9 left you looking at the cab, unable to see where you walked.** The game
  only switches to the walking camera in its developer mode, which players normally
  have off; 0.17 was only ever tried with it on. The plugin now switches developer
  mode on for the walk and puts it back afterwards.
- If the game still does not give the camera, the walk is called off with a message
  instead of leaving you walking blind.
- F9 no longer reaches the game while you are on foot.

## 0.17 (first public release)

For Euro Truck Simulator 2 1.61.1.1, Windows, 64-bit, single player.

- Get out of the parked truck with F9 and walk in first person: walk, run, crouch,
  jump, head bob and footsteps.
- The ground, walls, buildings, parked cars, signs and poles are solid around you,
  not only around the truck.
- The invisible walls behind the red X symbols do not stop you on foot.
- Get back in with F9 at the driver's door, or hold F9 from anywhere. A panel in the
  bottom left corner shows which of the two applies where you stand.
- Driving keys are kept away from the truck while you walk.
- The speedometer and the driver's figure are hidden while you are on foot.
- Settings in `takeawalk.ini`.
- `install.bat` finds the game through Steam and installs the plugin;
  `uninstall.bat` removes it.

Known limits are listed in the [README](README.md#known-limits).
