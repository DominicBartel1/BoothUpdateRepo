quick install for nozzle and RFID fix

cd /
rm -r BoothUpdateRepo-main
rm main.zip
curl -L -O https://github.com/DominicBartel1/BoothUpdateRepo/archive/refs/heads/main.zip
unzip main.zip
cd /BoothUpdateRepo-main
chmod u+x install
./install
