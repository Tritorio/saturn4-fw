SATURN 4 ULTRA 16K - COMPATIBILITY CHECK (step 1 of the beta test)
===================================================================
You are asked to run ONE read-only script on your own PC and send its text output. Nothing is installed on your printer, nothing is
changed, and NO firmware is provided at this stage.

WHY WE DO THIS
A firmware add-on for the Elegoo Saturn 4 Ultra 16K (firmware V1.5.9) was built and tested on ONE printer, the author's. Elegoo does
not sell one identical machine: the stock firmware carries drivers for several Wi-Fi chips, and units differ in their electronics
(for example the processor module, the memory chips, the FPGA / display module, the stepper drivers). Flashing software made for one
board revision onto another could, in the worst case, leave a printer that does not start. That is a remote possibility, but it is
not acceptable to take it on someone else's printer.

So the procedure is:
  1. You run the script below and send the output.            <- this folder
  2. The author compares your printer with the one it was made on: same stock firmware files? same flash layout? same electronics
     revision? (the script prints the checksums of the stock files so that this can be seen exactly)
  3. Only if everything matches, a test version may be offered to you, with its own instructions and a way back to Elegoo's firmware.
     If something differs, you get an answer and nothing is flashed.
Please do NOT flash anything on the basis of this page, and do not ask for the firmware before your report was compared.

WHAT THE SCRIPT DOES
It reads a few files through the printer's own web server (port 3030, the one the Elegoo app and slicer talk to; it shows the file
system read-only without a login, on the stock firmware too). It prints: the processor and flash layout, the eMMC maker and type,
the USB devices (Wi-Fi chip, camera), the FPGA / stepper driver lines of the app log and the md5 checksums of the app, the FPGA file
and the firmware copy kept inside the printer. It does not write, delete or start anything on the printer.
The text holds NO personal data: MAC addresses are masked; there is no serial number, Wi-Fi name or password, file name or printer
ID. Read the report before you send it; example_report.txt shows what it looks like.

WHAT YOU NEED
- the printer switched on, idle (not printing), on the same network as your PC (Wi-Fi or cable)
- the printer's IP address: on the printer's own settings screen, or in your router's list of devices
- Python 3 on the PC (python.org; nothing else to install)

HOW TO RUN IT
1. Put board_report_http.py in a folder and open a terminal / command prompt there.
2. Run it (replace the number with your printer's IP):
       Windows:      py board_report_http.py 192.168.1.50 > report.txt
       Linux / Mac:  python3 board_report_http.py 192.168.1.50 > report.txt
   It takes about 20 to 60 seconds. It prints one line to the screen while it checks the 30 MB firmware copy. Add  --no-image  to skip
   that check (then say so when you send the report).
3. Open report.txt and look at it. If it only contains "cannot reach", check the IP and that the printer and PC are on the same network.

WHAT TO SEND BACK
- report.txt (paste the text)
- the firmware version your printer's screen shows
- where and when you bought the printer (shop, month and year) and the revision / model text on its label, WITHOUT the serial number
Do not send your Wi-Fi details, a printer ID or photos showing them.

Thank you: every unit that is compared makes it safer for the next owner.
