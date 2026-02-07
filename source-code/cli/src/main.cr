require "option_parser"

def main
  home = ENV["HOME"]
  hackerland_path = "#{home}/.hackeros/hackerland/hackerland"

  update = false
  config = false

  parser = OptionParser.new do |parser|
    parser.banner = "Usage: hackerland [options]"

    parser.on("--help", "Show this help") do
      puts parser
      exit
    end

    parser.on("--update", "Update (placeholder, not implemented yet)") do
      update = true
    end

    parser.on("--config", "Run configuration mode") do
      config = true
    end

    parser.invalid_option do |flag|
      STDERR.puts "ERROR: #{flag} is not a valid option."
      STDERR.puts parser
      exit(1)
    end
  end

  parser.parse

  if update
    puts "Update not implemented yet."
    exit
  elsif config
    ENV["WAYLAND_DISPLAY"] = "wayland-99"
    Process.exec(hackerland_path, ["--config"])
  else
    # Default behavior
    ENV["WAYLAND_DISPLAY"] = "wayland-99"
    Process.exec(hackerland_path)
  end
end

main
