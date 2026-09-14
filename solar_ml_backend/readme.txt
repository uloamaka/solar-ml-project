HOW TO USE THIS APP
===================
1. Make sure Python is installed on your Raspberry PI
2. Open this folder in Visual Studio
3. Go to the terminal
4. Run this command in the terminal to create a virtual environment:
Windows: 
python -m venv .venv
macOS/Linux: 
python3 -m venv .venv

5. Activate the virtual environment with:

WindowsCommand Prompt (cmd):

venv\Scripts\activate.bat

macOS / Linuxbash / zsh source: 

venv/bin/activate

6. Run this command in the terminal to install all dependencies:

pip install -r requirements.txt

7. Run your fastapi app with:

fastapi dev main.py



