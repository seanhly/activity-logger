compile:
	gcc -o al al.c -lm
install:
	# If root, install to /usr/bin/
	if [ "$$(id -u)" -eq 0 ]; then \
		cp al /usr/bin/; \
		echo "Installed to /usr/bin/"; \
	else \
		cp al ${HOME}/.local/bin/; \
		echo "Installed to ${HOME}/.local/bin/"; \
	fi
uninstall:
	# If root, remove from /usr/bin/
	if [ "$$(id -u)" -eq 0 ]; then \
		rm -f /usr/bin/al; \
		echo "Removed from /usr/bin/"; \
	else \
		rm -f ${HOME}/.local/bin/al; \
		echo "Removed from ${HOME}/.local/bin/"; \
	fi
clean:
	rm -f al
