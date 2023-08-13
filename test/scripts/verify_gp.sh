#!/bin/bash
# Taken from verify_s5.sh


if ! command -v realpath &> /dev/null
then
    realpath() {
        [[ $1 = /* ]] && echo "$1" || echo "$PWD/${1#./}"
    }
fi

# Get the path of this script
SCRIPTPATH=$(realpath  $(dirname "$0"))

ZSTD=""
REGIONS=""
SAMPLES=""
ZSTD_LEVEL=""
unset -v NO_KEEP

POSITIONAL=()
while [[ $# -gt 0 ]]
do
key="$1"

# Command line argument parsing from :
# https://stackoverflow.com/questions/192249/how-do-i-parse-command-line-arguments-in-bash
case $key in
    -f|--filename)
    FILENAME="$2"
    shift # past argument
    shift # past value
    ;;
    -r|--regions)
    REGIONS="-r $2"
    shift # past argument
    shift # past value
    ;;
    -s|--samples)
    SAMPLES="-s $2"
    shift # past argument
    shift # past value
    ;;
    --zstd)
    ZSTD="--zstd"
    shift # past argument
    ;;
    --zstd-level)
    ZSTD_LEVEL="--zstd-level $2"
    shift # past argument
    shift # past value
    ;;
    --no-keep)
    NO_KEEP="YES"
    shift # past argument
    ;;
    *)    # unknown option
    POSITIONAL+=("$1") # save it in an array for later
    shift # past argument
    ;;
esac
done
set -- "${POSITIONAL[@]}" # restore positional parameters

if [ -z "${FILENAME}" ]
then
    echo "Specify a filename with --filename, -f <filename>"
    exit 1
fi

echo "FILENAME        = ${FILENAME}"

TMPDIR=$(mktemp -d -t xsi_XXXXXX) || { echo "Failed to create temporary directory"; exit 1; }

echo "Temporary director : ${TMPDIR}"
echo "Region : ${REGIONS}"
echo "Samples : ${SAMPLES}"

function exit_fail_rm_tmp {
    echo "Removing directory : ${TMPDIR}"
    rm -r ${TMPDIR}
    exit 1
}

"${SCRIPTPATH}"/../../xsqueezeit --GP -c ${ZSTD} ${ZSTD_LEVEL} -f ${FILENAME} -o ${TMPDIR}/compressed.xsi || { echo "Failed to compress ${FILENAME}"; exit_fail_rm_tmp; }
"${SCRIPTPATH}"/../../xsqueezeit -x ${REGIONS} ${SAMPLES} -f ${TMPDIR}/compressed.xsi -o ${TMPDIR}/uncompressed.bcf || { echo "Failed to uncompress ${FILENAME}"; exit_fail_rm_tmp; }

command -v bcftools || { echo "Failed to find bcftools, is it installed ?"; exit_fail_rm_tmp; }

echo
diff <(bcftools view ${REGIONS} ${SAMPLES} ${FILENAME}) <(bcftools view ${TMPDIR}/uncompressed.bcf) > ${TMPDIR}/difflog.txt
DIFFLINES=$(wc -l ${TMPDIR}/difflog.txt | awk '{print $1}')
#echo $DIFFLINES
if [ ${DIFFLINES} -gt 4 ]
then
    if [ -z "${NO_KEEP}" ]
    then
        echo
        echo "[KO] The files differ, check out ${TMPDIR}/difflog.txt"
        exit 1
    else
        exit_fail_rm_tmp # For unit testing
    fi
    exit 1
else
    echo
    echo "[OK] The files are the same"
fi

rm -r $TMPDIR
exit 0