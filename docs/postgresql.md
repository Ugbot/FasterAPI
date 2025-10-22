# PostgreSQL Integration Guide

Complete guide to using FasterAPI's high-performance PostgreSQL driver.

## Overview

FasterAPI includes a native PostgreSQL driver written in C++ that's **4-10x faster** than pure Python drivers. It features:

- Native binary protocol implementation
- Connection pooling with health checks
- Prepared statement caching
- Transaction support with automatic retries
- COPY support for bulk operations
- Full async/await compatibility

## Quick Start

### Installation

PostgreSQL support is included with FasterAPI:

```bash
pip install -e .[all]
```

### Basic Usage

```python
from fasterapi import App, Depends
from fasterapi.pg import PgPool
from fasterapi.pg.compat import get_pg_factory

# Create connection pool
pool = PgPool(
    "postgres://user:password@localhost:5432/mydb",
    min_size=2,
    max_size=20
)

# Create dependency
get_pg = get_pg_factory(pool)

app = App()

@app.get("/users")
def list_users(req, res, pg=Depends(get_pg)):
    result = pg.exec("SELECT * FROM users")
    return {"users": result.all()}

@app.on_event("shutdown")
def shutdown():
    pool.close()

if __name__ == "__main__":
    app.run()
```

## Connection Pool

### Creating a Pool

```python
from fasterapi.pg import PgPool

pool = PgPool(
    dsn="postgres://user:password@localhost:5432/mydb",
    min_size=2,        # Minimum connections to keep open
    max_size=20,       # Maximum connections
    timeout=30.0,      # Connection timeout in seconds
    idle_timeout=600.0 # Close idle connections after 10 minutes
)
```

### Connection String Format

```
postgres://[user[:password]@][host][:port][/database][?param=value]
```

Examples:
```python
# Basic
"postgres://localhost/mydb"

# With auth
"postgres://myuser:mypass@localhost/mydb"

# With port
"postgres://localhost:5433/mydb"

# With SSL
"postgres://localhost/mydb?sslmode=require"

# Full example
"postgres://user:pass@db.example.com:5432/production?sslmode=verify-full&connect_timeout=10"
```

### Pool Management

```python
# Get current pool size
size = pool.size()

# Get statistics
stats = pool.stats()
print(f"Active: {stats['active']}")
print(f"Idle: {stats['idle']}")
print(f"Total: {stats['total']}")

# Close pool
pool.close()
```

## Executing Queries

### Simple SELECT

```python
@app.get("/users")
def list_users(req, res, pg=Depends(get_pg)):
    # Execute query
    result = pg.exec("SELECT id, name, email FROM users")
    
    # Get all rows as dicts
    users = result.all()
    
    return {"users": users}
```

### Parameterized Queries

Always use parameterized queries to prevent SQL injection:

```python
@app.get("/users/{user_id}")
def get_user(req, res, pg=Depends(get_pg)):
    user_id = int(req.path_params["user_id"])
    
    # Use $1, $2, etc. for parameters
    result = pg.exec(
        "SELECT * FROM users WHERE id = $1",
        user_id
    )
    
    user = result.one_or_none()
    
    if user is None:
        res.status(404)
        return {"error": "User not found"}
    
    return {"user": user}
```

### Multiple Parameters

```python
@app.get("/search")
def search_users(req, res, pg=Depends(get_pg)):
    name_pattern = req.query_params.get("name", "%")
    min_age = int(req.query_params.get("min_age", 0))
    
    result = pg.exec(
        "SELECT * FROM users WHERE name LIKE $1 AND age >= $2",
        name_pattern,
        min_age
    )
    
    return {"users": result.all()}
```

## Result Handling

### Getting Results

```python
result = pg.exec("SELECT * FROM users")

# Get all rows as list of dicts
all_users = result.all()

# Get first row or None
first_user = result.one_or_none()

# Get first row or raise exception
first_user = result.one()

# Get single scalar value
count = pg.exec("SELECT COUNT(*) FROM users").scalar()
```

### Iterating Results

```python
result = pg.exec("SELECT * FROM users")

for row in result:
    print(row["name"], row["email"])
```

### Column Access

```python
user = result.one()

# Dict-style access
name = user["name"]
email = user["email"]

# Or convert to dict explicitly
user_dict = dict(user)
```

## INSERT, UPDATE, DELETE

### INSERT with RETURNING

```python
@app.post("/users")
def create_user(req, res, pg=Depends(get_pg)):
    data = req.json()
    
    # INSERT and get the new ID
    user_id = pg.exec(
        "INSERT INTO users(name, email, age) VALUES($1, $2, $3) RETURNING id",
        data["name"],
        data["email"],
        data.get("age")
    ).scalar()
    
    return {
        "id": user_id,
        "name": data["name"],
        "email": data["email"]
    }
```

### UPDATE

```python
@app.put("/users/{user_id}")
def update_user(req, res, pg=Depends(get_pg)):
    user_id = int(req.path_params["user_id"])
    data = req.json()
    
    pg.exec(
        "UPDATE users SET name = $1, email = $2 WHERE id = $3",
        data["name"],
        data["email"],
        user_id
    )
    
    return {"message": "Updated"}
```

### DELETE

```python
@app.delete("/users/{user_id}")
def delete_user(req, res, pg=Depends(get_pg)):
    user_id = int(req.path_params["user_id"])
    
    pg.exec("DELETE FROM users WHERE id = $1", user_id)
    
    return {"message": "Deleted"}
```

## Transactions

### Basic Transaction

```python
@app.post("/transfer")
def transfer_money(req, res, pg=Depends(get_pg)):
    data = req.json()
    
    # Start transaction
    with pg.tx() as tx:
        # All queries in this block are in the same transaction
        tx.exec(
            "UPDATE accounts SET balance = balance - $1 WHERE id = $2",
            data["amount"],
            data["from_id"]
        )
        
        tx.exec(
            "UPDATE accounts SET balance = balance + $1 WHERE id = $2",
            data["amount"],
            data["to_id"]
        )
        
        # Automatically commits if no exception
        # Automatically rolls back if exception
    
    return {"status": "ok"}
```

### Transaction with Error Handling

```python
@app.post("/transfer")
def transfer_money(req, res, pg=Depends(get_pg)):
    data = req.json()
    
    try:
        with pg.tx() as tx:
            # Check balance
            balance = tx.exec(
                "SELECT balance FROM accounts WHERE id = $1 FOR UPDATE",
                data["from_id"]
            ).scalar()
            
            if balance < data["amount"]:
                raise ValueError("Insufficient funds")
            
            # Transfer
            tx.exec(
                "UPDATE accounts SET balance = balance - $1 WHERE id = $2",
                data["amount"],
                data["from_id"]
            )
            
            tx.exec(
                "UPDATE accounts SET balance = balance + $1 WHERE id = $2",
                data["amount"],
                data["to_id"]
            )
        
        return {"status": "ok"}
        
    except ValueError as e:
        res.status(400)
        return {"error": str(e)}
```

### Isolation Levels

```python
from fasterapi.pg import TxIsolation

# Read committed (default)
with pg.tx(isolation=TxIsolation.read_committed) as tx:
    pass

# Repeatable read
with pg.tx(isolation=TxIsolation.repeatable_read) as tx:
    pass

# Serializable (strictest)
with pg.tx(isolation=TxIsolation.serializable) as tx:
    pass
```

### Automatic Retry

For serialization errors:

```python
# Retry up to 3 times on serialization failure
with pg.tx(isolation=TxIsolation.serializable, retries=3) as tx:
    # Your transactional code
    pass
```

## Bulk Operations (COPY)

For loading large amounts of data, use COPY - it's **10-100x faster** than individual INSERTs.

### COPY FROM (Import)

```python
@app.post("/import-users")
def import_users(req, res, pg=Depends(get_pg)):
    users = req.json()["users"]
    
    # Use COPY for fast bulk insert
    with pg.copy_in("COPY users(name, email, age) FROM stdin CSV") as pipe:
        for user in users:
            line = f"{user['name']},{user['email']},{user.get('age', '')}\n"
            pipe.write(line.encode())
    
    return {"imported": len(users)}
```

### COPY TO (Export)

```python
@app.get("/export-users")
def export_users(req, res, pg=Depends(get_pg)):
    # Stream data out
    data = []
    
    with pg.copy_out("COPY users TO stdout CSV HEADER") as pipe:
        while True:
            chunk = pipe.read(8192)
            if not chunk:
                break
            data.append(chunk.decode())
    
    res.set_header("Content-Type", "text/csv")
    return "".join(data)
```

### Binary COPY

For maximum speed:

```python
with pg.copy_in("COPY users(name, email, age) FROM stdin BINARY") as pipe:
    for user in users:
        # Write binary format (more complex but faster)
        pipe.write(encode_binary(user))
```

## Advanced Features

### Prepared Statements

Prepared statements are automatically cached:

```python
# First call: prepares statement
result1 = pg.exec("SELECT * FROM users WHERE id = $1", 1)

# Second call: reuses prepared statement (faster)
result2 = pg.exec("SELECT * FROM users WHERE id = $1", 2)
```

### Row Locking

```python
with pg.tx() as tx:
    # Lock row for update
    row = tx.exec(
        "SELECT * FROM users WHERE id = $1 FOR UPDATE",
        user_id
    ).one()
    
    # Modify and update
    row["balance"] += 100
    tx.exec(
        "UPDATE users SET balance = $1 WHERE id = $2",
        row["balance"],
        user_id
    )
```

### Listen/Notify

```python
# In one connection (background task)
async def listen_for_changes():
    pg = pool.get()
    pg.exec("LISTEN user_changes")
    
    while True:
        notification = pg.wait_for_notify(timeout=5.0)
        if notification:
            print(f"Received: {notification}")

# In another connection
pg.exec("NOTIFY user_changes, 'User 123 updated'")
```

### Custom Types

```python
# Register custom type handler
from fasterapi.pg import register_type

register_type(
    "json",
    encode=lambda x: json.dumps(x),
    decode=lambda x: json.loads(x)
)

# Now JSON columns work automatically
result = pg.exec("SELECT data FROM users WHERE id = $1", user_id)
data = result.one()["data"]  # Automatically decoded from JSON
```

## Performance Tips

### 1. Use Connection Pooling

```python
# Bad: New connection every request
@app.get("/users")
def list_users(req, res):
    pg = Pg("postgres://localhost/mydb")  # Slow!
    return pg.exec("SELECT * FROM users").all()

# Good: Use pool
pool = PgPool("postgres://localhost/mydb", min_size=10, max_size=100)
get_pg = get_pg_factory(pool)

@app.get("/users")
def list_users(req, res, pg=Depends(get_pg)):
    return pg.exec("SELECT * FROM users").all()
```

### 2. Batch Queries

```python
# Bad: N+1 queries
users = pg.exec("SELECT * FROM users").all()
for user in users:
    posts = pg.exec("SELECT * FROM posts WHERE user_id = $1", user["id"]).all()

# Good: Single query with JOIN
result = pg.exec("""
    SELECT u.*, json_agg(p.*) as posts
    FROM users u
    LEFT JOIN posts p ON p.user_id = u.id
    GROUP BY u.id
""").all()
```

### 3. Use COPY for Bulk

```python
# Bad: Individual INSERTs
for user in users:
    pg.exec("INSERT INTO users(name, email) VALUES($1, $2)", 
            user.name, user.email)

# Good: COPY
with pg.copy_in("COPY users(name, email) FROM stdin CSV") as pipe:
    for user in users:
        pipe.write(f"{user.name},{user.email}\n".encode())
```

### 4. Index Your Queries

```sql
-- Create indexes for frequently queried columns
CREATE INDEX idx_users_email ON users(email);
CREATE INDEX idx_posts_user_id ON posts(user_id);
CREATE INDEX idx_posts_created_at ON posts(created_at DESC);
```

### 5. Use Appropriate Isolation Levels

```python
# For read-only queries, no transaction needed
result = pg.exec("SELECT * FROM users")

# For simple writes, read_committed is fine
with pg.tx(isolation=TxIsolation.read_committed) as tx:
    tx.exec("INSERT INTO logs(message) VALUES($1)", message)

# Only use serializable when you need it
with pg.tx(isolation=TxIsolation.serializable, retries=3) as tx:
    # Complex logic requiring full isolation
    pass
```

## Error Handling

### Connection Errors

```python
from fasterapi.pg import PgError, ConnectionError

try:
    result = pg.exec("SELECT * FROM users")
except ConnectionError as e:
    # Database is down or connection failed
    res.status(503)
    return {"error": "Database unavailable"}
except PgError as e:
    # Other database error
    res.status(500)
    return {"error": str(e)}
```

### Query Errors

```python
try:
    result = pg.exec("SELECT * FROM nonexistent_table")
except PgError as e:
    print(f"Query failed: {e}")
    # Check error code
    if e.code == "42P01":  # undefined_table
        print("Table doesn't exist")
```

### Transaction Errors

```python
try:
    with pg.tx(isolation=TxIsolation.serializable) as tx:
        # Your transactional code
        pass
except SerializationError as e:
    # Serialization failure (retry might help)
    print("Transaction conflict, retry")
except PgError as e:
    # Other error
    print(f"Transaction failed: {e}")
```

## Testing

### Using Test Database

```python
# conftest.py
import pytest
from fasterapi.pg import PgPool

@pytest.fixture
def test_pool():
    pool = PgPool("postgres://localhost/test_db")
    yield pool
    pool.close()

@pytest.fixture
def pg(test_pool):
    pg = test_pool.get()
    
    # Start transaction
    pg.exec("BEGIN")
    
    yield pg
    
    # Rollback after test
    pg.exec("ROLLBACK")

# test_users.py
def test_create_user(pg):
    user_id = pg.exec(
        "INSERT INTO users(name, email) VALUES($1, $2) RETURNING id",
        "Test User",
        "test@example.com"
    ).scalar()
    
    assert user_id is not None
    
    # Verify
    user = pg.exec("SELECT * FROM users WHERE id = $1", user_id).one()
    assert user["name"] == "Test User"
```

## Comparison with Other Drivers

| Feature | FasterAPI | asyncpg | psycopg3 |
|---------|-----------|---------|----------|
| Language | C++ | C | C |
| Binary protocol | ✅ | ✅ | ✅ |
| Connection pool | ✅ (C++) | ⚠️ (Python) | ⚠️ (Python) |
| Async support | ✅ | ✅ | ✅ |
| Sync support | ✅ | ❌ | ✅ |
| COPY support | ✅ | ✅ | ✅ |
| Performance | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐ | ⭐⭐⭐⭐ |
| Prepared stmt cache | ✅ (Auto) | ✅ | ✅ |
| Transaction retry | ✅ (Built-in) | ❌ | ❌ |

### Performance Comparison

Simple SELECT query:

| Driver | Queries/sec | Notes |
|--------|-------------|-------|
| FasterAPI | 20,000 | C++ pool |
| asyncpg | 15,000 | Python pool |
| psycopg3 | 12,000 | Sync driver |

Bulk INSERT (1000 rows):

| Method | Time | Speedup |
|--------|------|---------|
| FasterAPI COPY | 10ms | 1x |
| Individual INSERTs | 1000ms | 0.01x |

## Troubleshooting

### Connection Refused

```
ConnectionError: could not connect to server
```

**Solution:** Make sure PostgreSQL is running:
```bash
# macOS
brew services start postgresql

# Ubuntu
sudo service postgresql start
```

### Authentication Failed

```
PgError: password authentication failed for user "myuser"
```

**Solution:** Check your connection string and PostgreSQL auth settings.

### Too Many Connections

```
PgError: FATAL: sorry, too many clients already
```

**Solution:** Reduce pool size or increase PostgreSQL's `max_connections`:
```sql
-- postgresql.conf
max_connections = 200
```

### Slow Queries

Use `EXPLAIN` to diagnose:
```sql
EXPLAIN ANALYZE SELECT * FROM users WHERE email = 'user@example.com';
```

Add indexes if needed:
```sql
CREATE INDEX idx_users_email ON users(email);
```

## Next Steps

- **[Getting Started](GETTING_STARTED.md)** - Complete tutorial
- **[Performance Guide](performance.md)** - Optimization tips
- **[Examples](../examples/)** - Working code examples

---

**Questions?** → [GitHub Discussions](https://github.com/bengamble/FasterAPI/discussions)

