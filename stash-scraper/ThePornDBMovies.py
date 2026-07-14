#!/usr/bin/env python3
import sys
import json
import os
import configparser
import urllib.request
import urllib.parse

# Get script directory to locate config.ini reliably
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG_PATH = os.path.join(SCRIPT_DIR, "config.ini")

def log(msg):
    print(f"[*] {msg}", file=sys.stderr)

# 1. Read API token from config.ini
config = configparser.ConfigParser()
if os.path.exists(CONFIG_PATH):
    config.read(CONFIG_PATH)
    API_TOKEN = config.get("tpdb", "api_token", fallback=None)
else:
    API_TOKEN = None

if not API_TOKEN:
    log("Error: config.ini or api_token not configured. Please create config.ini in the scraper directory.")
    sys.exit(1)

# 2. Parse command arguments
if len(sys.argv) < 2:
    log("Error: Action argument required (search or url)")
    sys.exit(1)

action = sys.argv[1]

# 3. Read input from stdin
try:
    input_data = json.loads(sys.stdin.read())
except Exception as e:
    log(f"Error parsing stdin JSON: {e}")
    sys.exit(1)

headers = {
    "Authorization": f"Bearer {API_TOKEN}",
    "Accept": "application/json",
    "User-Agent": "Stash-TPDB-Movies-Scraper/1.0"
}

def query_api(url):
    req = urllib.request.Request(url, headers=headers)
    try:
        with urllib.request.urlopen(req) as response:
            return json.loads(response.read().decode())
    except urllib.error.HTTPError as e:
        log(f"HTTP Error {e.code}: {e.reason}")
        return None
    except Exception as e:
        log(f"Request Error: {e}")
        return None

def scrape_movie_by_id(movie_id, url=None):
    if not url:
        url = f"https://theporndb.net/movies/{movie_id}"
    api_url = f"https://api.theporndb.net/movies/{movie_id}"
    response = query_api(api_url)
    
    if response and "data" in response:
        data = response["data"]
        
        # Populate movie/group name lists
        movie_name = data.get("title")
        movies_array = [{"name": "Movies"}]
        
        # Map performers
        performers = []
        for perf in data.get("performers", []):
            if perf.get("name"):
                performers.append({"name": perf.get("name")})
                
        # Map studio
        studio_data = None
        if data.get("studio") and data["studio"].get("name"):
            studio_data = {"name": data["studio"]["name"]}
            
        # Map director
        director_name = None
        directors = [d["name"] for d in data.get("directors", []) if d.get("name")]
        if directors:
            director_name = ", ".join(directors)
            
        # Map tags
        tags = []
        for tag in data.get("tags", []):
            if tag.get("name"):
                tags.append({"name": tag.get("name")})
            
        # Format the ScrapedScene output payload
        scraped_scene = {
            "title": movie_name,
            "details": data.get("description"),
            "date": data.get("date"),
            "url": url,
            "image": data.get("image"),
            "studio": studio_data,
            "performers": performers,
            "director": director_name,
            "tags": tags,
            # Dual-compatible group keys (older Stash versions use 'movies', newer use 'groups')
            "movies": movies_array,
            "groups": movies_array
        }
        return scraped_scene
    return None

if action == "search":
    # Stash sends search query under "name"
    query = input_data.get("name")
    if not query:
        log("No query name provided.")
        print(json.dumps([]))
        sys.exit(0)
        
    # Clean search query (strip extensions, tags, dots, extra spaces)
    if query:
        import re
        if query.lower().endswith(('.mp4', '.mkv', '.avi', '.wmv', '.mov', '.flv')):
            query = os.path.splitext(query)[0]
        query = re.sub(r'\b(720p|1080p|2160p|4k|sd|hd|hevc|x264|x265|h264|h265|bluray|rip|web|dl)\b', '', query, flags=re.IGNORECASE)
        query = re.sub(r'[\.\-_]', ' ', query)
        query = re.sub(r'\s+', ' ', query).strip()
        
    log(f"Searching for movie: '{query}'")
    encoded_query = urllib.parse.quote(query)
    api_url = f"https://api.theporndb.net/movies?q={encoded_query}"
    
    response = query_api(api_url)
    results = []
    
    if response and "data" in response:
        movies = response["data"]
        # If response["data"] is a single movie rather than a list, wrap it
        if isinstance(movies, dict):
            movies = [movies]
            
        for movie in movies:
            title = movie.get("title")
            movie_id = movie.get("id")
            if title and movie_id:
                results.append({
                    "title": title,
                    "url": f"https://theporndb.net/movies/{movie_id}"
                })
    print(json.dumps(results))

elif action in ("url", "query"):
    url = input_data.get("url")
    if not url:
        log("No URL provided.")
        print(json.dumps({}))
        sys.exit(0)
        
    log(f"Scraping Movie URL/Query: {url}")
    # Extract the Movie UUID/ID from the URL
    parts = url.strip("/").split("/")
    movie_id = parts[-1]
    
    scraped_scene = scrape_movie_by_id(movie_id, url)
    if scraped_scene:
        print(json.dumps(scraped_scene))
    else:
        log("No data returned from movie endpoint.")
        print(json.dumps({}))
        sys.exit(0)

elif action == "fragment":
    # 1. Check if there is a PornDB movie URL in urls/url
    url = None
    if input_data.get("url") and "theporndb.net/movies/" in input_data["url"]:
        url = input_data["url"]
    if not url and input_data.get("urls"):
        for u in input_data["urls"]:
            if u and "theporndb.net/movies/" in u:
                url = u
                break
                
    if url:
        log(f"Fragment has movie URL: {url}")
        parts = url.strip("/").split("/")
        movie_id = parts[-1]
        scraped_scene = scrape_movie_by_id(movie_id, url)
        if scraped_scene:
            print(json.dumps(scraped_scene))
            sys.exit(0)
            
    # 2. If no URL, try to search strictly by the scene's Title field
    query = input_data.get("title")
            
    # Clean up common video tags and release identifiers for all fragment queries
    if query:
        import re
        # Clean extension if it snuck into the title
        if query.lower().endswith(('.mp4', '.mkv', '.avi', '.wmv', '.mov', '.flv')):
            query = os.path.splitext(query)[0]
        query = re.sub(r'\b(720p|1080p|2160p|4k|sd|hd|hevc|x264|x265|h264|h265|bluray|rip|web|dl)\b', '', query, flags=re.IGNORECASE)
        query = re.sub(r'[\.\-_]', ' ', query) # replace dots/dashes/underscores with space
        query = re.sub(r'\s+', ' ', query).strip() # clean extra spaces
            
    if not query:
        log("No Title found for fragment search.")
        print(json.dumps({}))
        sys.exit(0)
        
    log(f"Fragment search for movie: '{query}'")
    encoded_query = urllib.parse.quote(query)
    api_url = f"https://api.theporndb.net/movies?q={encoded_query}"
    
    response = query_api(api_url)
    if response and "data" in response:
        movies = response["data"]
        if isinstance(movies, dict):
            movies = [movies]
            
        if movies:
            first_movie = movies[0]
            movie_id = first_movie.get("id")
            if movie_id:
                log(f"Fragment match found first movie: '{first_movie.get('title')}' (ID: {movie_id})")
                scraped_scene = scrape_movie_by_id(movie_id)
                if scraped_scene:
                    print(json.dumps(scraped_scene))
                    sys.exit(0)
                    
    log("No movie details could be resolved for fragment.")
    print(json.dumps({}))
    sys.exit(0)

else:
    log(f"Unknown action: {action}")
    sys.exit(1)
