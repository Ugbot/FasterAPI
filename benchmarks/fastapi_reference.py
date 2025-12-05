#!/usr/bin/env python3.13
"""
Reference FastAPI implementation for benchmarking.

This is a standard FastAPI application without FasterAPI optimizations,
used as a baseline for performance comparison.
"""

from fastapi import FastAPI
from pydantic import BaseModel
from typing import Optional
import uvicorn

app = FastAPI(title="FastAPI Reference")

class Item(BaseModel):
    name: str
    description: Optional[str] = None
    price: float
    tax: Optional[float] = None

# In-memory data
items_db = {
    1: {"id": 1, "name": "Widget", "price": 29.99},
    2: {"id": 2, "name": "Gadget", "price": 49.99},
    3: {"id": 3, "name": "Doohickey", "price": 19.99},
}

@app.get("/")
def read_root():
    return {"message": "Hello FastAPI"}

@app.get("/health")
def health_check():
    return {"status": "healthy"}

@app.get("/items")
def list_items(skip: int = 0, limit: int = 10):
    items = list(items_db.values())
    return items[skip:skip + limit]

@app.get("/items/{item_id}")
def get_item(item_id: int):
    if item_id not in items_db:
        return {"error": "Not found"}
    return items_db[item_id]

@app.post("/items")
def create_item(item: Item):
    new_id = max(items_db.keys()) + 1
    item_data = item.dict()
    item_data["id"] = new_id
    items_db[new_id] = item_data
    return {"id": new_id, "item": item_data}

if __name__ == "__main__":
    print("\n" + "="*80)
    print("FastAPI Reference Server (for benchmarking)")
    print("="*80)
    print("Starting on http://0.0.0.0:8001")
    print("="*80 + "\n")

    uvicorn.run(app, host="0.0.0.0", port=8001, log_level="error")
