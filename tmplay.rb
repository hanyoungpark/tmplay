class Tmplay < Formula
  desc "Terminal video player with Kitty/iTerm2/block rendering and audio support"
  homepage "https://github.com/hanyoungpark/tmplay"
  url "https://github.com/hanyoungpark/tmplay/archive/refs/tags/v1.0.0.tar.gz"
  sha256 "PLACEHOLDER_SHA256"  # run: brew fetch --build-from-source Formula/tmplay.rb
  license "MIT"

  depends_on "cmake" => :build
  depends_on "ffmpeg"
  depends_on "ftxui"
  depends_on :macos

  def install
    system "cmake", "-S", ".", "-B", "build",
           "-DCMAKE_BUILD_TYPE=Release",
           *std_cmake_args
    system "cmake", "--build", "build"
    bin.install "build/tmplay"
  end

  test do
    assert_match "Usage", shell_output("#{bin}/tmplay --help 2>&1", 1)
  end
end
