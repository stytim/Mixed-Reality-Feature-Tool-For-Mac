# Mixed Reality Feature Tool for Mac

## Overview
At the moment, if you want to use Mixed Reality Toolkit 3 (MRTK 3) for Unity on a Mac, you would have to download the Mixed Reality Feature Tool on a Windows machine to configure the Unity project. However, MRTK is a cross-platform Unity library, so there is no reason why someone using a Mac should have to use a Windows machine just to configure the Unity project with the Mixed Reality Feature Tool.

The Mixed Reality Feature Tool for Mac is a command-line tool that allows developers to download the necessary MRTK3 components on macOS. It currently does not have a graphical user interface (GUI) and runs in the terminal.

This tool is provided in both C++ and Python versions.

## C++ Version

### Build
To build the C++ version, follow these steps:
```bash
git clone https://github.com/stytim/Mixed-Reality-Feature-Tool-For-Mac.git mrfeaturetool
cd mrfeaturetool
mkdir build && cd build
cmake ..
make
```

### Usage
To use the C++ version, run in your terminal from the build directory:
```bash
./mrfeature_tool path-to-your-Unity-project
```

## Python Version 

### Requirements 
Install the required packages using pip:
```bash
pip install requests packaging
```
### Usage 
To use the Python version, run in your terminal: 
```bash
python mrfeature_tool.py path-to-your-Unity-project 
```

## Tool Interaction 

The tool will try to retrive the list of the MRTK 3 components like this:
<p align="left">
	<img width="80%" src="image/usage.png">
</p>

Select the MRTK3 components you want to import by typing the index numbers separated by a space. Press Enter to finish.
The tool will automatically download the selected components with the latest version, along with their dependencies.

## Plan
Adding a GUI


## License
The Mixed Reality Feature Tool for Mac is licensed under the MIT License.
