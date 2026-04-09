import delta_sharing
import os

# Load credentials from .env
endpoint = None
token = None
with open(".env") as f:
    for line in f:
        if line.startswith("DELTA_SHARING_ENDPOINT="):
            endpoint = line.split("=")[1].strip()
        if line.startswith("DELTA_SHARING_BEARER_TOKEN="):
            token = line.split("=")[1].strip()

# Create profile
profile_content = f"""{{
    "shareCredentialsVersion": 1,
    "endpoint": "{endpoint}",
    "bearerToken": "{token}"
}}"""

# Try to load the table
table_url = endpoint + "#prequel_dev_share.prequel_dev.orders"
try:
    data = delta_sharing.load_as_pandas(table_url)
    print("SUCCESS: Loaded table")
    print(data.head())
except Exception as e:
    print(f"FAILURE: {e}")
