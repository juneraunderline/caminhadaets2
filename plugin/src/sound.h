/**
 * @brief Sounds of the walker. They are generated when the plugin starts, so no audio files are needed.
 */

#pragma once

namespace sound {

void init(void);
void shutdown(void);

/**
 * @brief Plays one footstep, a slightly different one each time.
 */
void footstep(void);

/**
 * @brief Plays the heavier thud of landing after a jump or a fall.
 */
void landing(void);

/** Plays the door opening/closing followed by the seatbelt when returning to the cab. */
void truck_entered(void);

/** Plays the door opening/closing when leaving the cab. */
void truck_exited(void);

/** Plays only the seatbelt when the game first starts inside the cab. */
void startup_buckle(void);

} // namespace sound
