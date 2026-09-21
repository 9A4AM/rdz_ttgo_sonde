# nginx image serving the OTA update artifacts (update.ino.bin / update.fs.bin).
#
# The firmware .bin files are NOT baked in — they change on every build. Bind-mount
# the build's ota folder at run time instead:
#
#   docker build -t rdz-ota ota-server
#   docker run -d --name rdz-ota -p 80:80 \
#     -v "$PWD/.pio/build/ttgo-lora32/ota:/usr/share/nginx/html:ro" rdz-ota
#
# (The Makefile ota-serve / ota-stop targets wrap these commands.)
FROM nginx:1.27-alpine
COPY nginx.conf /etc/nginx/conf.d/default.conf
EXPOSE 80
