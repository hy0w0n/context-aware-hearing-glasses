import os
from dotenv import load_dotenv
from openai import OpenAI

load_dotenv()

client = OpenAI(
    api_key=os.getenv("AI_GATEWAY_API_KEY"),
    base_url=os.getenv("AI_GATEWAY_BASE_URL"),
)

models = client.models.list()

for model in models.data:
    print(model.id)
