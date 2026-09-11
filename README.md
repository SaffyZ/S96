hallo this is PS96 or PlaySaffy 96, basically a PS1 emulator in C++20, this is an early version which will be published because people wouldn't stop bugging me
not much works on it
it has a Dashboard that was made by chatgpt so credits to him ig
Claude tried making it before him and messed up a ton soo some stuff might be buggy... I don't know if you'll even run games on this but yeah
the internet taught me how a ton of people might be mean while a ton might be wholesome
that being said i am never publishing the latest PS96
or ever publishing my work again
you can call this AI, you can call this shit
i geniunely do not care

controls:
for games u just use arrow keys for the D pad I for Cross J for Circle K for Square L for Triangle A and D for L1 L2 Q and E for R1 R2 Enter for Start and Right Shift for Select W and S are saved for later stuff (making a 2 player mode)

when adding games you have to make sure that there's a folder called Games in the S96 directory
and then inside that is for example Tekken 3 folder
inside that is a .cue and .bin
and there's cover.png that's optional

also no illegal bios or games duhh
OpenBIOS doesn't work on this version for some reason..

how to build
windows (MSYS2 UCRT64)
if ur on windows open powershell and run this

on PS :

PowerShell
$Env:Path += ";C:\msys64\ucrt64\bin"
cd C:\Users\Saffy\Downloads\S96\S96
Remove-Item -Recurse -Force build -ErrorAction SilentlyContinue
cmake -B build
cmake --build build
.\build\ps96.exe -bios "C:\Users\Saffy\Downloads\SCPH5501.bin"
dependencies:

PowerShell
pacman -S mingw-w64-ucrt-x86_64-SDL2 mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-gcc
PS :

PowerShell
.\build\ps96.exe -bios "C:\path\to\SCPH5501.bin" -disc "C:\path\to\game.cue"
u can also throw in extra flags when u launch it like -debug which prints some logs inside the build folder it logs ur FPS GPU draws texture counts controller traffic and reset events u can also use -gpu-trace if u wanna record the full GP0 command stream for debugging
