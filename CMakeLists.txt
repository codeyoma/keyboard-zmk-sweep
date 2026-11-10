# Code in this repository, built into the firmware as a Zephyr module (see zephyr/module.yml).

if(CONFIG_ZMK_MOUSE_SPEED)
  target_sources(app PRIVATE src/mouse_speed.c)
endif()

# KeyTray: active layer and battery levels for the KeyTray menu bar app
if(CONFIG_ZMK_KEYTRAY)
  target_sources(app PRIVATE src/keytray.c)
endif()
