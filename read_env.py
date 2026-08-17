import os
import sys
from SCons.Script import DefaultEnvironment

env = DefaultEnvironment()

# Läs .env-filen om den finns
if os.path.exists(".env"):
    with open(".env") as f:
        for line in f:
            line = line.strip()
            # Hoppa över tomma rader och kommentarer
            if not line or line.startswith("#") or "=" not in line:
                continue
                
            key, value = line.split("=", 1)
            key = key.strip()
            value = value.strip().strip('"').strip("'")
            
            # Sätt variabeln i operativsystemets miljö för denna process
            os.environ[key] = value
            
            # Sätt variabeln i PlatformIOs interna byggmiljö (sysenv)
            env["ENV"][key] = value