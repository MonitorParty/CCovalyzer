import os
import sqlite3

conn = sqlite3.connect('queue.db')
cur = conn.cursor()
cur.execute('''
            CREATE TABLE IF NOT EXISTS queue_cases (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                filename TEXT,
                data BLOB
                )
            ''')
conn.commit()


def insert_tc(filename, blob):
    cur.execute('INSERT INTO queue_cases (filename, data) VALUES (?, ?)',
                (filename, blob))
    conn.commit()
    

#hardcode, sorry
queue_folder = "/data/playground/AFLplusplus-hb-testrun/setup-hb/out/default/queue/"
count = 0
for fname in os.listdir(queue_folder):
    fpath = os.path.join(queue_folder, fname)
    if os.path.isfile(fpath) and "execs:0," in fname:
        with open(fpath, 'rb') as f:
            data = f.read()
            insert_tc(fname, data)
            print(f"Inserted #{count}")
            count = count + 1


