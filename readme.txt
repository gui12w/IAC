Well, this codec is imprecise, uses 13 to 14 bit precision (just like txac)

It has 4 executables, each one doing it's own thing
The input: takes wav files (16 bits or 32 bits little-endian) and transforms into iac without any loss.
The output: takes the iac file and transforms it back to wav (if you want to check how is the quality in audacity or smth)
The player: takes the iac file and play it for you without any new file
The player exclusive: does the exact same thing as the player, but plays in exclusive wasapi mode, but you still need to manually set the sample rate on windows to match the source, or he'll resample it

It's a lossless codec, can have a 0,0012 db imprecision (bet you can hear it), it uses metadata and seek table, also, it supports multi-channel decode and encode,this codec uses delta encoding and rice encoding

Commands for each executable:
iacinput: iacinput <input_audio> <output.iac>
iacoutput: iacoutput <input.iac> <output.wav>
iacplay: iacplay <file.iac>
iacplaye: iacplaye <file.iac>

And yes, you can put ffmpeg in the PATH and use it with any audio source (it does NOT contain any ffmpeg code, it only puts a command line in cmd)

Updates can be check on the documentation
(i'll try to change the compression method in future updates)

iac_playexclusive.c and iac_play.c has the qoaplay.c as a base (which the base is txac)
creator of the qoaplay.c and the QOA codec: Dominic Szablewski