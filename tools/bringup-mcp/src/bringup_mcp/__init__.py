"""bringup-mcp: an MCP server that fronts `tools/bringup/bu` for a remote agent.

One code path runs every command (`runner.run_bu`), one gate decides every actuate call
(`gate.decide`), and every tool is generated from `bu schema` at startup so the server
cannot drift from the CLI. See README.md and DECISIONS.md.
"""

__version__ = "0.1.3"
