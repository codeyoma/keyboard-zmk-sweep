# Code in this repository, built into the firmware as a Zephyr module (see zephyr/module.yml).

if(CONFIG_ZMK_MOUSE_SPEED)
  target_sources(app PRIVATE src/mouse_speed.c)
endif()
