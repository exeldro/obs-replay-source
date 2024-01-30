# Replay Source for OBS Studio

Plugin for OBS Studio to (slow motion) replay async sources from memory.

Download from
https://obsproject.com/forum/resources/replay-source.686/
https://github.com/exeldro/obs-replay-source/releases

Unzip the download in the 64 bit plugins folder for example:
C:\Program Files\obs-studio\
or
C:\Program Files (x86)\obs-studio\

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
* **Load delay**
Delay in milliseconds before the replay is loaded.
* **Maximum replays**
Maximum number of replays to keep in memory.
* **Video source**
The source that has the (async) replay filter to retrieve the video (and audio) data from.
* **Capture internal frames**
The async replay filter to retrieve the internal video frames to be able to get higher fps.
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
* **Progress crop source**
The right side of the source gets cropped by the percentage of the posistion in the current replay
* **Text source**
Text source that gets the text formatted by the next setting
* **Text format**
Text for the Text source
  * **%SPEED%**
  * **%PROGRESS%**
  * **%COUNT%** 
  * **%INDEX%**
  * **%DURATION%**
  * **%TIME%**
* **Sound trigger load replay**
Enable sound trigger for loading replays
* **Threshold db**
The threshold above which the audio must peak to trigger the loading of a new replay
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
Decrease the speed by 33%
* **Normal or faster**
If speed is < 100% set speed to 100% else increase the speed by 50%
* **Normal or slower**
If speed is > 100% set speed to 100% else decrease the speed by 33%
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
* **Disable next scene**
Disable the automatic next scene switching function.
* **Enable next scene**
Enable the automatic next scene switching function.
* **Previous frame**
Rewind the playback head one frame. Automatically pauses playback.
* **Next frame**
Advance the playback head one frame. Automatically pauses playback.
* **Step backward N frames**
Rewind the playback head N frames. Automatically pauses playback. Set N in source settings.
* **Step forward N frames**
Advance the playback head N frames. Automatically pauses playback. Set N in source settings.
