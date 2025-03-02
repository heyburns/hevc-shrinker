#!/usr/bin/env bash
# list_processed.sh - Lists files already processed (recorded in processed_files.db).

DB_FILE="processed_files.db"

# Check if the database file exists.
if [ ! -f "$DB_FILE" ]; then
  echo "Error: Database file '$DB_FILE' not found."
  exit 1
fi

echo "Already processed files:"
sqlite3 "$DB_FILE" "SELECT filepath FROM processed_files ORDER BY processed_at DESC;"
