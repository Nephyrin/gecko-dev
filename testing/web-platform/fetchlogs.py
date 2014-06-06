import argparse
import cStringIO
import gzip
import json
import os
import requests
import urlparse

treeherder_base = "http://treeherder-dev.allizom.org/"

def create_parser():
    parser = argparse.ArgumentParser()
    parser.add_argument("branch", action="store",
                        help="Branch on which jobs ran")
    parser.add_argument("commit",
                        action="store",
                        help="Commit hash for push")

    return parser

def download(url, prefix, dest, force_suffix=True):
    if dest is None:
        dest = "."

    if prefix and not force_suffix:
        name = os.path.join(dest, prefix + ".log")
    else:
        name = None
    counter = 0

    while not name or os.path.exists(name):
        counter += 1
        sep = "" if not prefix else "-"
        name = os.path.join(dest, prefix + sep + str(counter) + ".log")

    with open(name, "w") as f:
        resp = requests.get(url)
        #temp_f = cStringIO.StringIO(resp.text.encode(resp.encoding))
        #gzf  = gzip.GzipFile(fileobj=temp_f)
        #f.write(gzf.read())
        f.write(resp.text.encode(resp.encoding))

def get_blobber_url(branch, job):
    job_id = job["id"]
    resp = requests.get(urlparse.urljoin(treeherder_base,
                                         "/api/project/%s/artifact/?job_id=%i&name=Job%%20Info" % (branch,
                                                                                                   job_id)))
    job_data = resp.json()
    print job_data
    if job_data:
        assert len(job_data) == 1
        job_data = job_data[0]
        try:
            link = job_data["blob"]["tinderbox_printlines"][1]
            prefix = "<a href='"
            assert link.startswith(prefix)
            link = link[len(prefix):]
            return link[:link.find("'")]
        except:
            return None


def get_structured_logs(branch, commit, dest=None):
    resp = requests.get(urlparse.urljoin(treeherder_base, "/api/project/%s/resultset/?revision=%s" % (branch,
                                                                                                      commit)))
    job_data = resp.json()

    for result in job_data["results"]:
        for platform in result["platforms"]:
            for group in platform["groups"]:
                for job in group["jobs"]:
                    if job["job_type_name"] == "W3C Web Platform Tests":
                        url = get_blobber_url(branch, job)
                        if url:
                            prefix = job["platform"]
                            download(url, prefix, None)

def main():
    parser = create_parser()
    args = parser.parse_args()

    get_structured_logs(args.branch, args.commit)

if __name__ == "__main__":
    main()
