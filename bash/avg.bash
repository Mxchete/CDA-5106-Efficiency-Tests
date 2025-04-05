avg() {
  local f=${1:-1}
  awk -F "${2:- }" "length(\$$f) { i+=1; sum+=\$$f; } END { print sum/i }"
}
