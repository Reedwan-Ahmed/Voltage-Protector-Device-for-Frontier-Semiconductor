# Voltage-Protector-Device-for-Frontier-Semiconductor
This repository contains the firmware, simulation files, and documentation for a microcontroller-based Voltage Protector system designed for the Frontier Semiconductor Embedded System Engineer assessment.

## Project Overview
The system continuously monitors emulated grid voltage and load current. It protects connected appliances by physically isolating the load via a relay if the grid drops below 180V, surges above 250V, or exceeds a 10A current draw.

## How to Run the Simulation
1. Open the Proteus project file located in the `project_files/` directory.
2. Double-click the ATmega328P microcontroller in the schematic.
3. Load the compiled `.hex` file (also located in `project_files/`) into the microcontroller.
4. Run the simulation.
5. Use the RV1 (Voltage) and RV2 (Current) potentiometers to simulate grid fluctuations and trigger the fault logic.

## Interface and Control
* **Local UI (4-Button Matrix):** Use the hardware buttons to navigate the 16x2 LCD menu and adjust safety thresholds directly on the device. The 4 buttons function as follows:
  * **Menu:** Interrupts the main screen and enters the Settings menu.
  * **Up:** Increases the selected safety limit (Voltage/Current).
  * **Down:** Decreases the selected safety limit.
  * **Save:** Saves the new limit permanently to the microcontroller's EEPROM.
* **Remote UI:** Connect via the UART Virtual Terminal to view live telemetry and send remote configuration commands.

## Documentation
Please refer to the `design document.pdf` in the root directory for a detailed breakdown of the hardware architecture, firmware logic, and testing results.
