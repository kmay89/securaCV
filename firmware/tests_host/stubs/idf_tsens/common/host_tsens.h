/* The chip's die-temperature sensor, as the fake IDF drivers beside this file
 * answer it, for test_camera_thermal.cpp (sweep F432). One struct serves both
 * driver APIs the shared provider (securacv_thermal.cpp) compiles against:
 * ESP-IDF 4.4's driver/temp_sensor.h (esp32s3_idf4/, the Arduino 2.0.17 core
 * of the stock units) and ESP-IDF 5's driver/temperature_sensor.h
 * (esp32s3_idf5/, the pioarduino core 3 of [env:full]). The test sets what
 * the driver answers and reads what the provider asked of it. */
#ifndef STUB_IDF_TSENS_HOST_TSENS_H
#define STUB_IDF_TSENS_HOST_TSENS_H

struct HostTsens {
  // What the fake driver answers, each in force until the test clears it.
  bool install_fails = false;  // IDF 5: temperature_sensor_install(); IDF 4.4: temp_sensor_set_config()
  bool enable_fails = false;   // IDF 5: temperature_sensor_enable();  IDF 4.4: temp_sensor_start()
  bool read_fails = false;     // the celsius read answers an error, the output untouched
  float celsius = 40.0f;       // what a read that succeeds answers

  // What the provider asked of it.
  int installs = 0, enables = 0, disables = 0, uninstalls = 0, reads = 0;
  bool installed = false, enabled = false;
  int range_min = 0, range_max = 0;   // IDF 5's configured measurement range
  int dac_offset = -1, clk_div = -1;  // IDF 4.4's configured offset and divider
};
inline HostTsens& host_tsens() {
  static HostTsens t;
  return t;
}

#endif
