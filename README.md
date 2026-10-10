# Voltage-Protector-Device-for-Frontier-Semiconductor (Md Reedwan Ahmed)
This repository contains the firmware, simulation files, and documentation for a microcontroller-based Voltage Protector system designed for the Frontier Semiconductor Embedded System Engineer assessment.

## Project Overview
The system continuously monitors emulated grid voltage and load current. It protects connected appliances by physically isolating the load via a relay if the grid drops below 180V, surges above 250V, or exceeds a 10A current draw.

## How to Run the Simulation
1. Open the Proteus project file **[REEDWAN_PROTEUS_PROJECT.pdsprj](./project_files/REEDWAN_PROTEUS_PROJECT.pdsprj)** located in the **[project_files/](./project_files)** directory.
2. Double-click the ATmega328P microcontroller in the schematic and Select the Program File folder.
3. Load the compiled hex file, **[voltage_protector.ino.hex](./src/voltage_protector.ino.hex)** (located in the **[src/](./src)** directory), into the microcontroller properties.
4. Run the simulation.
5. Use the RV1 (Voltage) and RV2 (Current) potentiometers to simulate grid fluctuations and trigger the fault logic.

## Interface and Control
* **Local UI (4-Button Matrix):** Use the hardware buttons to navigate the 16x2 LCD menu and adjust safety thresholds directly on the device. The 4 buttons function as follows:
  * **Menu:** Interrupts the main screen and enters the Settings menu.
  * **Up:** Increases the selected safety limit (Voltage/Current).
  * **Down:** Decreases the selected safety limit.
  * **Save:** Saves the new limit permanently to the microcontroller's internal EEPROM.
* **Remote UI:** Connect via the UART Virtual Terminal to view live telemetry and send remote configuration commands.

## Documentation & Evidence
Project Report **[Project Report (design_document.pdf)](./design_document.pdf)**. 

To view the raw circuit diagrams or screenshots of the system handling faults, please check the **[schematics/](./schematics)** and **[test_evidence/](./test_evidence)** folders respectively.

## Repository Contents
You can click directly on the links below to access the project files and directories:
* **[project_files/REEDWAN_PROTEUS_PROJECT.pdsprj](./project_files/REEDWAN_PROTEUS_PROJECT.pdsprj)** - The main Proteus simulation workspace.
* **[src/voltage_protector.ino.hex](./src/voltage_protector.ino.hex)** - The compiled firmware hex file to upload to the microcontroller.
* **[src/SourceCode_for_verify_only/SourceCode_for_verify_only.ino](./src/SourceCode_for_verify_only/SourceCode_for_verify_only.ino)** - The original Arduino IDE source code from which the compiled hex file was extracted.
* **[design_document.pdf](./design_document.pdf)** - The 1-page Project Report detailing the design approach, components, and results.
* **[schematics/](./schematics)** - Contains the system circuit diagrams and visual block diagrams.
* **[test_evidence/](./test_evidence)** - View performance captures, logs, and screenshots of the simulation in action.
