# Replay Source for OBS Studio

Plugin for OBS Studio to (slow motion) replay async sources from memory.

Download from
https://obsproject.com/forum/resources/replay-source.686/
https://github.com/exeldro/obs-replay-source/releases

Unzip the download in the 64 bit plugins folder for example:
C:\Program Files (x86)\obs-studio\obs-plugins\64bit

Start OBS Studio 64 bit
Add a replay source.
Set the source, duration and speed in the properties.
Set the replay hotkey.

# Replay source
This source replays audio and video after it is retrieved from a replay filter by using the hotkey or the button in the properties.
# (async) replay filter
Keeps the configured seconds from a source in memory. The name of the filter must be the same as the replay source used to play the replay.
The async version captures audio and video, the non async version only captures video.
## Properties
* **Duration**
Amount of seconds the replay needs to keep in memory.
* **Maximum replays**
Maximum number of replays to keep in memory.
* **Video source**
The source that has the (async) replay filter to retrieve the video (and audio) data from.
* **Audio source**
The source that has the replay audio filter to retrieve the audio data from.
* **Visibility Action**
The action that should be taken when the replay source becomes active (visible in output) or deactivates.
  * **Restart**
Start the replay from the beginning on becoming active and pause on deactivating.
  * **Pause**
Resume play on activating and pause on deactivating
  * **Continue**
Resume play on activation
  * **None**
No action taken on (de)activation
* **Start delay**
Delay in milliseconds before the replay starts playing.
* **End action**
The action that should be taken when the replay has finished playing.
  * **Hide**
After the replay the source will show nothing
  * **Pause**
After the replay the source will keep pause and keep showing the last video frame.
  * **Loop**
After the replay restart the replay.
  * **Reverse**
After the replay reverse the playing direction of the replay.
* **Next scene**
The scene that should be shown after the replay has finished playing.
Leave empty if you do not want automatic scene switching.
* **Speed percentage**
The speed that the replay should be played. 100 for normal speed. 50 for half speed.
* **Backward**
Start playing replays backwards.
* **Directory**
Directory to save replays to.
* **Filename formatting**
Formatting used to generate a filename for the replay (%CCYY-%MM-%DD %hh.%mm.%ss)
* **Lossless**
Use lossless avi or flv format saving the replay.
## hotkeys
* **Load replay**
Retrieve the replay.
* **Next**
Play the next replay.
* **Previous**
Play the previous replay.
* **First**
Play the first replay.
* **Last**
Play the last replay.
* **Remove**
Remove the current replay
* **Clear**
Remove all replays
* **Save replay**
Save the current replay disk.
* **Restart**
Play the current replay from the beginning.
* **Pause**
Pauses the replay, freezes the video.
* **Faster**
Increase the speed by 50%
* **Slower**
Decrease the speed bij 33%
* **Normal speed**
Set the speed to 100%
* **Half speed**
Set the speed to 50%
* **Double speed**
Set the speed to 200%
* **Reverse**
Start playing in the reverse direction
* **Forward**
Start playing forward
* **Backward**
Start playing backward
* **Trim front**
Remove all video before the current position from the replay
* **Trim end**
Remove all video after the current position from the replay
* **Trim reset**
Undo all trimming done on the replay.
* **Disable**
Disable the capturing of replays, removes the replay filters.
* **Enable**
Enable the capturing of replays, adds the replay filters.
