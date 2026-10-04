# Copyright (c) 2026 SugaBlaz
# This software is released under the MIT License.
# https://github.com/SugaBlaz/PRJ_SUGA-27-ANDROID-TERMUX

# modterm.py
import shlex
import sys
import inspect
import os
import platform
from typing import Optional, Union, List, get_args, get_origin

# Standard Linux/Android readline support for auto-completion and navigation
try:
    import readline
except ImportError:
    readline = None

import cli.logger as logger

class ModularTerminal:
    def __init__(self, prompt="sbcli: user> "):
        self.prompt = prompt
        self.commands = {}
        self.aliases = {}

        # Core Commands
        self.create_command("exit", self._exit, aliases=["quit", "q", "return"])
        self.create_command("help", self._show_help, aliases="?")
        self.create_command("theme", logger.change_theme)
        self.create_command("clear", self._clear_screen, aliases="cls")
        self.create_command("lsdir", self._list_directory, aliases=["ls", "dir"])
        self.create_command("log", self._log)
        self.create_command("themes", self._get_themes, aliases=["vt", "gt"])

        # Feature Commands
        self.create_command("cd", self._change_directory)
        self.create_command("pwd", self._print_working_directory, aliases=["gl"])
        self.create_command("sysinfo", self._system_info, aliases="info")
        self.create_command("alias", self._create_alias, aliases=["aliases", "shortcut"])
        self.create_command("change_local_username", self.change_localuser, aliases=["changelocaluser", "localuser"])

        # Initialize Auto-Completion for Termux
        if readline:
            readline.set_completer(self._completer)
            readline.parse_and_bind("tab: complete")

    def _completer(self, text, state):
        """Provides auto-completion for commands and all aliases."""
        options = [
            cmd
            for cmd in list(self.commands.keys()) + list(self.aliases.keys())
            if cmd.startswith(text)
        ]
        if state < len(options):
            return options[state]
        return None

    def create_command(
        self,
        command: str,
        function: callable,
        aliases: Optional[Union[str, List[str]]] = None,
    ):
        """Registers a command and optional aliases."""
        sig = inspect.signature(function)

        # Strip 'self' parameter if registering a bound method
        params = list(sig.parameters.values())
        if params and params[0].name == "self":
            sig = sig.replace(parameters=params[1:])

        params_display = []
        has_variadic = False

        for name, param in sig.parameters.items():
            if param.kind == inspect.Parameter.VAR_POSITIONAL:
                params_display.append(f"[{name}... (OPTIONAL)]")
                has_variadic = True
                continue

            annotation = param.annotation
            type_name = "str"

            if annotation != inspect.Parameter.empty:
                if isinstance(annotation, str):
                    type_name = annotation
                else:
                    origin = get_origin(annotation)
                    if origin is Union or getattr(origin, "__name__", "") == "UnionType":
                        args = [
                            a
                            for a in get_args(annotation)
                            if getattr(a, "__name__", "") != "NoneType"
                        ]
                        if args:
                            type_name = getattr(args[0], "__name__", str(args[0]))
                    else:
                        type_name = getattr(annotation, "__name__", str(annotation))

            if param.default != inspect.Parameter.empty:
                default_val = f"={param.default}" if param.default is not None else ""
                params_display.append(f"[{name}: {type_name}{default_val} (OPTIONAL)]")
            else:
                params_display.append(f"<{name}: {type_name} (REQUIRED)>")

        cmd_name = command.strip().lower()
        alias_list = []
        if isinstance(aliases, str):
            alias_list = [aliases.strip().lower()]
        elif isinstance(aliases, (list, tuple, set)):
            alias_list = [a.strip().lower() for a in aliases]

        self.commands[cmd_name] = {
            "func": function,
            "help": function.__doc__ or "No description provided.",
            "params_display": params_display,
            "sig": sig,
            "has_variadic": has_variadic,
            "aliases": alias_list,
        }

        for alias in alias_list:
            self.aliases[alias] = cmd_name

    def _convert_type(self, value: str, expected_type):
        """Attempts to type-cast user string input tokens into specified function parameters."""
        if expected_type == inspect.Parameter.empty:
            return value

        if isinstance(expected_type, str):
            builtins = {"str": str, "int": int, "float": float, "bool": bool}
            expected_type = builtins.get(expected_type, str)

        origin = get_origin(expected_type)
        if origin is Union or getattr(origin, "__name__", "") == "UnionType":
            args = [
                a
                for a in get_args(expected_type)
                if getattr(a, "__name__", "") != "NoneType"
            ]
            if args:
                expected_type = args[0]
            else:
                return value

        if isinstance(expected_type, str):
            builtins = {"str": str, "int": int, "float": float, "bool": bool}
            expected_type = builtins.get(expected_type, str)

        if expected_type is bool:
            if value.lower() in ("true", "1", "yes", "on"):
                return True
            if value.lower() in ("false", "0", "no", "off"):
                return False
            raise ValueError("[-] Cannot convert the input into a boolean.")

        if expected_type is str:
            return str(value)

        return expected_type(value)

    def execute(self, user_input):
        if not user_input.strip():
            return

        try:
            parts = shlex.split(user_input)
            cmd_name = parts[0].lower()
            args = parts[1:]
        except ValueError as e:
            logger.log(f"[-] Parsing Error: {e}")
            return

        if cmd_name in self.aliases:
            cmd_name = self.aliases[cmd_name]

        if cmd_name in self.commands:
            cmd_data = self.commands[cmd_name]
            sig = cmd_data["sig"]

            try:
                if not cmd_data["has_variadic"]:
                    bound_args = sig.bind_partial(*args)
                    converted_args = {}

                    for param_name, raw_value in bound_args.arguments.items():
                        param_meta = sig.parameters[param_name]
                        try:
                            converted_args[param_name] = self._convert_type(
                                raw_value, param_meta.annotation
                            )
                        except (ValueError, TypeError):
                            annotation = param_meta.annotation
                            origin = get_origin(annotation)
                            if origin is Union or getattr(origin, "__name__", "") == "UnionType":
                                type_args = [
                                    a
                                    for a in get_args(annotation)
                                    if getattr(a, "__name__", "") != "NoneType"
                                ]
                                expected = type_args[0].__name__ if type_args else "str"
                            else:
                                expected = getattr(annotation, "__name__", str(annotation))

                            logger.log(
                                f"[-] Type Error: Argument '{param_name}' requires type '{expected}'. Received: '{raw_value}'."
                            )
                            return

                    final_bound = sig.bind_partial(**converted_args)
                    final_bound.apply_defaults()
                    cmd_data["func"](*final_bound.args, **final_bound.kwargs)
                else:
                    cmd_data["func"](*args)

            except TypeError as e:
                logger.log(f"[-] Argument Error for '{cmd_name}': {e}")
            except Exception as e:
                logger.log(f"[-] Runtime Error in '{cmd_name}': {e}")
        else:
            logger.log(f"[-] Unknown command: '{cmd_name}'. Type 'help' for info.")

        sys.stdout.flush()

    def run_loop(self):
        """Starts the interactive shell prompt loop."""
        logger.log("[+] Terminal initialized. Type 'help' for commands.")

        while True:
            try:
                sys.stdout.flush()
                user_input = input(self.prompt)
                self.execute(user_input)
            except (KeyboardInterrupt, EOFError):
                self._exit()

    def _show_help(self, *args):
        """Displays available commands along with all of their aliases and parameters."""
        logger.log("\n[*] Available Commands:")
        for cmd, data in sorted(self.commands.items()):
            alias_str = f" (aliases: {', '.join(data['aliases'])})" if data["aliases"] else ""
            param_str = " " + " ".join(data["params_display"]) if data["params_display"] else ""
            logger.log(f"  {cmd}{alias_str}{param_str}")

            if data.get("help"):
                for line in data["help"].splitlines():
                    logger.log(f"    {line}")
            logger.log("")

    def _exit(self, *args):
        """Exits the command line interface."""
        logger.log("\nExiting...")
        sys.exit(0)

    def _log(self, text: str, delay: Optional[float] = None):
        """Prints text using logger."""
        logger.log(text, delay=delay)

    def _clear_screen(self, *args):
        """Clears the terminal screen using native Linux/Android clear command."""
        os.system("clear")

    def _list_directory(self, path: Optional[str] = None):
        """Lists the files and folders in the current or target directory."""
        target_dir = path if path else "."
        try:
            entries = os.listdir(target_dir)
            for entry in sorted(entries):
                logger.log(entry)
        except FileNotFoundError:
            logger.log(f"[-] Directory not found: '{target_dir}'.")
        except PermissionError:
            logger.log(f"[-] Permission denied: '{target_dir}'.")

    def _change_directory(self, path: str):
        """Changes the current working directory."""
        try:
            os.chdir(path)
            logger.log(f"[+] Changed directory to: {os.getcwd()}")
        except FileNotFoundError:
            logger.log(f"[-] Directory not found: '{path}'.")

    def _print_working_directory(self):
        """Prints the current working directory path."""
        logger.log(os.getcwd())

    def _system_info(self):
        """Displays basic system and OS information."""
        uname = platform.uname()
        logger.log(f"System: {uname.system}")
        logger.log(f"Node: {uname.node}")
        logger.log(f"Release: {uname.release}")
        logger.log(f"Machine: {uname.machine}")

    def _create_alias(self, target_command: str = "", *alias_names: str):
        """Creates one or multiple shortcuts for an existing command."""
        if not target_command or not alias_names:
            logger.log("[-] Usage: alias <target_command> <alias1> [alias2 ...]")
            return

        target_clean = target_command.strip().lower()

        if target_clean in self.aliases:
            target_clean = self.aliases[target_clean]

        if target_clean not in self.commands:
            logger.log(f"[-] Target command '{target_command}' does not exist.")
            return

        added_aliases = []
        for alias_raw in alias_names:
            alias_clean = alias_raw.strip().lower()
            if not alias_clean:
                continue

            self.aliases[alias_clean] = target_clean

            if alias_clean not in self.commands[target_clean]["aliases"]:
                self.commands[target_clean]["aliases"].append(alias_clean)

            added_aliases.append(alias_clean)

        if added_aliases:
            formatted_aliases = ", ".join(f"'{a}'" for a in added_aliases)
            logger.log(f"[+] Alias(es) {formatted_aliases} registered for '{target_clean}'.")

    def _get_themes(self, columns: Optional[int] = 3):
        """Gets all the valid themes and prints them to the console."""
        themes = sorted(list(logger.VALID_COLORS))

        for i in range(0, len(themes), columns):
            row = themes[i : i + columns]
            logger.log("".join(f"{color:<16}" for color in row))
            
    def change_localuser(self, username: str):
        """Changes the local username displayed in the terminal prompt."""
        username = username.strip()

        if not username:
            logger.log("[-] Username cannot be empty.")
            return

        self.prompt = f"sbcli: {username}> "
        logger.log(f"[+] Local user changed to '{username}'.")