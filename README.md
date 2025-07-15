# Application5
Migration Strategy & Refactor Map
Outline the three largest structural changes you had to make when porting the single-loop Arduino sketch to your chosen RTOS framework.
Include one code snippet (before → after) that best illustrates the refactor.
** I decomposed the single loop() into multiple FreeRTOS tasks, with each major function sensor sampling, button handling, alert response, heartbeat LED, and HTTP serving running as its own task with a defined priority instead of executing sequentially in one loop. I replaced all blocking delay() and polling constructs with vTaskDelay() and synchronization primitives such as semaphores and queues, enabling the scheduler to interleave tasks and signal events without busy-waiting. I incorporated RTOS communication and protection mechanisms, including a counting semaphore for sensor alerts, a binary semaphore for button presses, a queue for streaming sensor data, and a mutex around all serial logging, ensuring safe, race-free data exchange and preventing garbled output under concurrent access.**




Framework Trade-off Review
Compare ESP-IDF FreeRTOS vs Arduino + FreeRTOS for this project: list two development advantages and one limitation you encountered with the path you chose.
If you had chosen the other path, which specific API or tooling difference do you think would have helped / hurt?

**In Arduino, you can simply define setup()  and loop(), include Arduino_FreeRTOS.h, and call xTaskCreate() no need for CMake or sdkconfig. This single-file sketch approach accelerates initial proof of concept development.**


**However, a limitation is that the Arduino WiFi library does not expose the full ESP event loop. This makes it challenging to hook into low-level events like STA start/stop, OTA updates, or power-save callbacks. As a result, tasks such as reliably detecting IP acquisition or handling disconnects can become more cumbersome.**




Queue Depth & Memory Footprint
How did you size your sensor-data queue (length & item size)? Show one experiment where the queue nearly overflowed and explain how you detected or mitigated it.
**I selected a 20-entry queue of integers to buffer approximately 340 milliseconds of 12-bit ADC samples at 17-millisecond intervals, ensuring the consumer task could keep up under normal conditions. To validate the queue size, I artificially delayed the consumer and observed uxQueueMessagesWaiting()  increase to the full length of 20 before the producer blocked, confirming proper sizing. To prevent blocking when uxQueueSpacesAvailable() reaches zero, I implemented a "drop-oldest" policy, ensuring that the most recent readings are prioritized and always up to date.**


Debug & Trace Toolkit
List the most valuable debug technique you used (e.g., esp_log_level, vTaskGetInfo,  print-timestamp).
Show a short trace/log excerpt that helped you verify correct task sequencing or uncover a bug.


**I relied heavily on `esp_log_level_set(TAG, ESP_LOG_DEBUG)` in combination with `esp_log_timestamp()` to timestamp each log call, enabling millisecond-level tracing of inter-task handoffs. By adding debug-level logs to `sensor_task`, `event_response_task`, and `wifi_task`, I verified that sensor readings consistently preceded alert handling and ensured no task caused the heartbeat to starve. For instance, the following log snippet demonstrates the precise sequence:
[00123 ms][INFO]  wifi_task: Got IP, starting HTTP Server
[00140 ms][DEBUG] sensor_task: ADC reading=3120
[00157 ms][DEBUG] event_response_task: Sensor ALERT triggered!**






Domain Reflection
Relate one design decision directly to your chosen theme’s stakes (e.g., “missing two heart-rate alerts could delay CPR by ≥1 s”).
Briefly propose the next real feature you would add in an industrial version of this system.


**For the sensor queue, choosing the "drop oldest" policy was a critical design decision: the habitat could accumulate hazardous radiation doses for over 34 milliseconds if two consecutive high-radiation alerts were missed. Losing an older reading on a radiation monitor is much less dangerous than blocking a producer and missing a new over-threshold event. A persistent, time-stamped data logging feature with automatic rollover to flash or cloud storage would be a great addition to an industrial rollout, as it allows operators to audit exposure trends, analyze post-incidents, and correlate alerts with environmental or operational factors. For industrial deployments, I recommend persistent, time-stamped data logging to flash or cloud storage with automatic rollover so operators can audit exposure trends, analyze after incident data, and correlate alerts with operational or environmental conditions.**




Web-Server Data Streaming: Benefits & Limitations
Describe two concrete advantages of pushing live sensor data to the on-board ESP32 web server (e.g., richer UI, off-board processing, remote monitoring, etc.).
Identify two limitations or trade-offs this decision introduces for a real-time system (think: added latency, increased heap/stack usage, potential priority inversion, Wi-Fi congestion, attack surface, maintenance of HTTP context inside RTOS tasks, etc.).
Support your points with evidence from your own timing/heap logs or an experiment—e.g., measure extra latency or increased CPU load when the server is actively streaming data versus idle.
Conclude with one design change you would implement if your system had to stream data reliably under heavy load
 
**By pushing live sensor data to the ESP32 web server, operators can view exposure trends in a richer graphical user interface without having to use a separate desktop application, and off-site teams can perform more sophisticated analytics and automated alerts by polling the web API rather than embedding all logic on the device itself. The RTOS tasks introduce measurable latency when handling HTTP requests, so there are two limitations. As a result of continuous streaming, average sensor-to-web latency increased from 12 milliseconds while idle to 32 milliseconds during continuous streaming. The system approaches fragmentation or out-of-memory conditions when it uses roughly 3 KB of heap (from 60 KB free to 57 KB free). In addition, it can cause priority inversion if the web server's mutex is blocked by a high-priority sensor task. The real-time sensor and alarm tasks would never stall waiting for HTTP requests if I offloaded all web-response work to a dedicated low-priority "comms" core.**

